#define _DEFAULT_SOURCE

#ifndef ULAPI
#error This is intended as a userspace HAL component only.
#endif

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <rtapi.h>
#include <hal.h>

/*
 * LinuxCNC driver for BMR SFU 0151 VFD serial control, 115200 baud, 8N1.
 */

#define PORT_DEFAULT        "/dev/ttyUSB0"
#define SERIAL_TIMEOUT_MS   500

#define COMMAND_START               0x24
#define RESPONSE_START              0xE4
#define COMMAND_STOP                0x25
#define RESPONSE_STOP               0xE5
#define COMMAND_STATUS              0x60
#define RESPONSE_STATUS             0xE0
#define COMMAND_GET_REQUESTED_SPEED 0x41
#define RESPONSE_GET_REQUESTED_SPEED 0xC1
#define COMMAND_GET_SPEED_OUTPUT    0x42
#define RESPONSE_GET_SPEED_OUTPUT   0xC2
#define COMMAND_GET_REF_SPEED       0x43
#define RESPONSE_GET_REF_SPEED      0xC3
#define COMMAND_SET_SPEED           0x01
#define RESPONSE_SET_SPEED          0xC1
#define COMMAND_SET_DIR_CW          0x0A
#define RESPONSE_SET_DIR_CW         0xCA
#define COMMAND_SET_DIR_CCW         0x0B
#define RESPONSE_SET_DIR_CCW        0xCB
#define COMMAND_SET_DP              0x0C
#define RESPONSE_SET_DP             0xCC

#define ADDR_CURRENT        0x0BB6
#define ADDR_VOLTAGE        0x0BD4
#define ADDR_OVR_CURRENT    0x08A0
#define ADDR_STATUS         0x0860

static int serial_fd = -1;
static volatile sig_atomic_t stop_requested = 0;
static int done = 0;

typedef struct {
    hal_bit_t *spindle_start;
    hal_bit_t *spindle_cw;
    hal_bit_t *spindle_ccw;
    hal_float_t *spindle_rpm;

    hal_bit_t *running;
    hal_bit_t *target_speed_reached;
    hal_bit_t *stopped;
    hal_bit_t *undervoltage;
    hal_bit_t *overvoltage;
    hal_bit_t *rs232_error;
    hal_bit_t *spindle_not_ready;
    hal_bit_t *converter_not_ready;
    hal_bit_t *overload;
    hal_bit_t *converter_overtemp;
    hal_bit_t *spindle_overtemp;
    hal_bit_t *comm_error;
    hal_u32_t *status_word;
    hal_float_t *current;
    hal_float_t *voltage;
    hal_u32_t *rpm_feedback;
    char *modname;
    char *port;
} haldata_t;

typedef struct {
    bool running;
    bool target_speed_reached;
    bool stopped;
    bool undervoltage;
    bool overvoltage;
    bool rs232_error;
    bool spindle_not_ready;
    bool converter_not_ready;
    bool overload;
    bool converter_overtemp;
    bool spindle_overtemp;
} SpindleStatus;

static void handle_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
    done = 1;
    printf("stop requested");
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int configure_serial(const char *port)
{
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", port, strerror(errno));
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "fcntl(F_SETFL, O_NONBLOCK): %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "tcgetattr(%s): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tty);

    if (cfsetispeed(&tty, B115200) != 0 || cfsetospeed(&tty, B115200) != 0) {
        fprintf(stderr, "cfset*speed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | CSTOPB);
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif

    /* Timeout is handled with poll() so reads have a total 500 ms deadline. */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "tcsetattr(%s): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int write_all(const uint8_t *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(serial_fd, data + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {
                    .fd = serial_fd,
                    .events = POLLOUT,
                    .revents = 0
                };
                if (poll(&pfd, 1, 50) <= 0) {
                    fprintf(stderr, "serial write: %s\n", strerror(errno));
                    if (serial_fd >= 0) {
                        close(serial_fd);
                        serial_fd = -1;
                    }
                    return -1;
                }
                continue;
            }
            fprintf(stderr, "serial write: %s\n", strerror(errno));
            if (serial_fd >= 0) {
                close(serial_fd);
                serial_fd = -1;
            }
            return -1;
        }
        sent += (size_t)n;
    }

    /* Equivalent to pyserial's flush(): wait until bytes have left the driver. */
    if (tcdrain(serial_fd) != 0) {
        if (errno != EIO && errno != ENOTTY && errno != EINVAL)
            fprintf(stderr, "tcdrain: %s\n", strerror(errno));
        if (serial_fd >= 0) {
            close(serial_fd);
            serial_fd = -1;
        }
        return -1;
    }

    return 0;
}

static int read_exact_timeout(uint8_t *buf, size_t len, int timeout_ms)
{
    size_t received = 0;
    int64_t deadline = monotonic_ms() + timeout_ms;

    while (received < len) {
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0)
            return 0; /* timeout */

        struct pollfd pfd = {
            .fd = serial_fd,
            .events = POLLIN,
            .revents = 0
        };

        int rc = poll(&pfd, 1, (int)remaining);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "poll: %s\n", strerror(errno));
            return -1;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "serial poll error: revents=0x%x\n", pfd.revents);
            return -1;
        }

        ssize_t n = read(serial_fd, buf + received, len - received);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            fprintf(stderr, "serial read: %s\n", strerror(errno));
            if (serial_fd >= 0) {
                close(serial_fd);
                serial_fd = -1;
            }
            return -1;
        }
        if (n == 0)
            continue;

        received += (size_t)n;
    }

    return 1;
}

/* Writes one command byte. */
static int write_command(uint8_t command)
{
    return write_all(&command, 1);
}

/* Writes one command byte followed by a little-endian uint16 value. */
static int write_value(uint8_t command, uint16_t value)
{
    uint8_t packet[3] = {
        command,
        (uint8_t)(value & 0xFF),
        (uint8_t)((value >> 8) & 0xFF)
    };

    return write_all(packet, sizeof(packet));
}

/* Reads response byte + little-endian uint16 data. */
static int read_value(uint8_t expected_response, uint16_t *value)
{
    uint8_t data[3];
    int rc = read_exact_timeout(data, sizeof(data), SERIAL_TIMEOUT_MS);

    if (rc == 0) {
        fprintf(stderr, "serial timeout waiting for response 0x%02X\n",
                expected_response);
        return -1;
    }
    if (rc < 0)
        return -1;

    if (data[0] != expected_response) {
        fprintf(stderr,
                "unexpected response 0x%02X, expected 0x%02X\n",
                data[0], expected_response);
        return -1;
    }

    *value = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    return 0;
}

static int read_word(uint8_t command, uint8_t response, uint16_t *value)
{
    if (write_command(command) != 0)
        return -1;
    return read_value(response, value);
}

static int write_word(uint8_t command, uint8_t response,
                      uint16_t value, uint16_t *returned_value)
{
    if (write_value(command, value) != 0)
        return -1;
    return read_value(response, returned_value);
}

static SpindleStatus get_status_bits(uint16_t status)
{
    SpindleStatus s = {
        .running = (status & (1u << 1)) != 0,
        .target_speed_reached = (status & (1u << 5)) != 0,
        .stopped = (status & (1u << 6)) != 0,
        .undervoltage = (status & (1u << 7)) != 0,
        .overvoltage = (status & (1u << 8)) != 0,
        .rs232_error = (status & (1u << 10)) != 0,
        .spindle_not_ready = (status & (1u << 11)) != 0,
        .converter_not_ready = (status & (1u << 12)) != 0,
        .overload = (status & (1u << 13)) != 0,
        .converter_overtemp = (status & (1u << 14)) != 0,
        .spindle_overtemp = (status & (1u << 15)) != 0
    };
    return s;
}
static int start_spindle(double spindle_rpm, bool spindle_cw)
{
    uint16_t response_value;
    if (write_word(COMMAND_SET_SPEED, RESPONSE_SET_SPEED, (uint16_t)(spindle_rpm / 10.0), &response_value) != 0)
        return -1;
    printf("Set Speed: %u\n", (unsigned)response_value * 10u);

    if (spindle_cw) {
        if (write_word(COMMAND_SET_DIR_CW, RESPONSE_SET_DIR_CW, 0, &response_value) != 0)
            return -1;
    } else {
        if (write_word(COMMAND_SET_DIR_CCW, RESPONSE_SET_DIR_CCW, 0, &response_value) != 0)
            return -1;
    }

    if (read_word(COMMAND_START, RESPONSE_START, &response_value) != 0)
        return -1;
    printf("Start: 0x%X\n", response_value);
    return 0;
}

static int stop_spindle(void)
{
    uint16_t value;
    if (read_word(COMMAND_STOP, RESPONSE_STOP, &value) != 0)
        return -1;

    printf("Stop: 0x%X\n", value);
    return 0;
}

static void set_comm_error(haldata_t *haldata, bool state)
{
    if (haldata != NULL && haldata->comm_error != NULL)
        *(haldata->comm_error) = state;
}

int main(int argc, char **argv)
{
    const char *modname = "bmr_sfu";
    const char *port = PORT_DEFAULT;
    int exit_code = EXIT_FAILURE;
    haldata_t *haldata;
    int comp_id;

    if (argc >= 2)
        port = argv[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        fprintf(stderr, "sigaction: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    comp_id = hal_init(modname);
    if (comp_id < 0) {
        fprintf(stderr, "bmr_sfu151: hal_init(%s) failed\n", modname);
        return EXIT_FAILURE;
    }

    haldata = (haldata_t *)hal_malloc(sizeof(*haldata));
    if (haldata == NULL) {
        fprintf(stderr, "bmr_sfu151: hal_malloc failed\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    memset(haldata, 0, sizeof(*haldata));
    haldata->modname = (char *)modname;
    haldata->port = (char *)port;

    if (hal_pin_bit_newf(HAL_IN, &(haldata->spindle_start), comp_id, "%s.start", modname) != 0) {
        fprintf(stderr, "bmr_sfu151: could not create start pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->spindle_start = 0;
    if (hal_pin_bit_newf(HAL_IN, &(haldata->spindle_cw), comp_id, "%s.spindle_cw", modname) != 0) {
        fprintf(stderr, "bmr_sfu151: could not create spindle_cw pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->spindle_cw = 1;
    if (hal_pin_bit_newf(HAL_IN, &(haldata->spindle_ccw), comp_id, "%s.spindle_ccw", modname) != 0) {
        fprintf(stderr, "bmr_sfu151: could not create spindle_ccw pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->spindle_ccw = 0;
    if (hal_pin_float_newf(HAL_IN, &(haldata->spindle_rpm), comp_id, "%s.spindle_rpm", modname) != 0) {
        fprintf(stderr, "bmr_sfu151: could not create spindle_rpm pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->spindle_rpm = 5000.0;

    if (hal_pin_bit_newf(HAL_OUT, &(haldata->running), comp_id, "%s.running", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create running pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->running = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->target_speed_reached), comp_id, "%s.target_speed_reached", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create target_speed_reached pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->target_speed_reached = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->stopped), comp_id, "%s.stopped", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create stopped pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->stopped = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->undervoltage), comp_id, "%s.undervoltage", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create undervoltage pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->undervoltage = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->overvoltage), comp_id, "%s.overvoltage", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create overvoltage pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->overvoltage = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->rs232_error), comp_id, "%s.rs232_error", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create rs232_error pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->rs232_error = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->spindle_not_ready), comp_id, "%s.spindle_not_ready", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create spindle_not_ready pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->spindle_not_ready = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->converter_not_ready), comp_id, "%s.converter_not_ready", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create converter_not_ready pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->converter_not_ready = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->overload), comp_id, "%s.overload", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create overload pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->overload = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->converter_overtemp), comp_id, "%s.converter_overtemp", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create converter_overtemp pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->converter_overtemp = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->spindle_overtemp), comp_id, "%s.spindle_overtemp", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create spindle_overtemp pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->spindle_overtemp = 0;
    if (hal_pin_u32_newf(HAL_OUT, &(haldata->status_word), comp_id, "%s.status_word", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create status_word pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->status_word = 0;
    if (hal_pin_float_newf(HAL_OUT, &(haldata->current), comp_id, "%s.current", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create current pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->current = 0.0;
    if (hal_pin_float_newf(HAL_OUT, &(haldata->voltage), comp_id, "%s.voltage", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create voltage pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->voltage = 0.0;
    if (hal_pin_u32_newf(HAL_OUT, &(haldata->rpm_feedback), comp_id, "%s.rpm_feedback", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create rpm_feedback pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->rpm_feedback = 0;
    if (hal_pin_bit_newf(HAL_OUT, &(haldata->comm_error), comp_id, "%s.comm_error", modname) != 0) {
        fprintf(stderr, "bmr_sfu_control: could not create comm_error pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->comm_error = 0;

    hal_ready(comp_id);

    serial_fd = configure_serial(port);
    if (serial_fd < 0) {
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }

    printf("Connected to %s @ 115200 baud\n", port);

    bool spindle_start;
    bool spindle_cw;
    bool spindle_ccw;
    double spindle_rpm;
    bool spindle_start_last = false;
    bool spindle_cw_last = true;
    double spindle_rpm_last = 5000;
    uint16_t response_value;
    bool request_start = false;
    uint16_t status_word;
    uint16_t current_raw;
    uint16_t voltage_raw;
    uint16_t speed_raw;
    SpindleStatus status;

    while (!done) {
        if (serial_fd < 0) {
            serial_fd = configure_serial(port);
            if (serial_fd < 0) {
                set_comm_error(haldata, true);
                if (haldata != NULL && haldata->running != NULL)
                    *(haldata->running) = 0;
                usleep(100000);
                continue;
            }
            printf("Reconnected to %s @ 115200 baud\n", port);
        }

        spindle_start = *(haldata->spindle_start);
        spindle_cw = *(haldata->spindle_cw);
        spindle_ccw = *(haldata->spindle_ccw);
        spindle_rpm = *(haldata->spindle_rpm);

        // Read status data
        if (write_word(COMMAND_SET_DP, RESPONSE_SET_DP, ADDR_STATUS, &status_word) != 0) {
            set_comm_error(haldata, true);
            *(haldata->running) = 0;
            usleep(10000);
            continue;
        }
        status = get_status_bits(status_word);
        *(haldata->running) = status.running;
        *(haldata->target_speed_reached) = status.target_speed_reached;
        *(haldata->stopped) = status.stopped;
        *(haldata->undervoltage) = status.undervoltage;
        *(haldata->overvoltage) = status.overvoltage;
        *(haldata->rs232_error) = status.rs232_error;
        *(haldata->spindle_not_ready) = status.spindle_not_ready;
        *(haldata->converter_not_ready) = status.converter_not_ready;
        *(haldata->overload) = status.overload;
        *(haldata->converter_overtemp) = status.converter_overtemp;
        *(haldata->spindle_overtemp) = status.spindle_overtemp;
        *(haldata->status_word) = status_word;

        if (write_word(COMMAND_SET_DP, RESPONSE_SET_DP, ADDR_CURRENT, &current_raw) != 0) {
            set_comm_error(haldata, true);
            continue;
        }
        if (write_word(COMMAND_SET_DP, RESPONSE_SET_DP, ADDR_VOLTAGE, &voltage_raw) != 0) {
            set_comm_error(haldata, true);
            continue;
        }
        if (read_word(COMMAND_GET_SPEED_OUTPUT, RESPONSE_GET_SPEED_OUTPUT, &speed_raw) != 0) {
            set_comm_error(haldata, true);
            continue;
        }
        *(haldata->current) = current_raw / 100.0;
        *(haldata->voltage) = voltage_raw / 10.0;
        *(haldata->rpm_feedback) = (unsigned)speed_raw * 10u;

        // Start pin changed
        if (spindle_start != spindle_start_last) {
            set_comm_error(haldata, false);
            spindle_start_last = spindle_start;
            if (spindle_start) {
                request_start = true;

            } else {
                if (stop_spindle() != 0) {
                    fprintf(stderr, "bmr_sfu: stop failed\n");
                    set_comm_error(haldata, true);
                    continue;
                }
            }
        }

        // The start command is only accepted when the spindle is stopped. So wait here for stop.
        if (request_start) {
            if (status.stopped) {
                if (start_spindle(spindle_rpm, spindle_cw) != 0) {
                    fprintf(stderr, "bmr_sfu: start failed\n");
                    set_comm_error(haldata, true);
                    request_start = false;
                    continue;
                }
                request_start = false;
            }
        }

        // Speed changed
        if (spindle_rpm != spindle_rpm_last) {
            spindle_rpm_last = spindle_rpm;
            if (write_word(COMMAND_SET_SPEED, RESPONSE_SET_SPEED, (uint16_t)(spindle_rpm / 10.0), &response_value) != 0) {
                fprintf(stderr, "bmr_sfu: set speed failed\n");
                set_comm_error(haldata, true);
                continue;
            }
            printf("Set Speed: %u\n", (unsigned)response_value * 10u);
        }

        // Direction pin changed
        if (spindle_cw != spindle_cw_last) {
            spindle_cw_last = spindle_cw;
            if (spindle_start) {
                if (stop_spindle() != 0) {
                    set_comm_error(haldata, true);
                    continue;
                }
                request_start = true;
            }
        }

        
        usleep(10000);
    }

    exit_code = EXIT_SUCCESS;

    stop_spindle();

    if (done) {
        printf("\nStopped\n");
    }

    if (serial_fd >= 0) {
        close(serial_fd);
        serial_fd = -1;
    }

    hal_exit(comp_id);
    return exit_code;
}
