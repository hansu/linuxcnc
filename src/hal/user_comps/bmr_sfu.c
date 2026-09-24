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
 * sudo halcompile --install --userspace bmr_sfu.c 
 * loadusr -W bmr_sfu /dev/ttyXXX 
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
    hal_float_t *loop_time;
    hal_float_t *ramp_rpm;
    hal_float_t *min_rpm;

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
    hal_float_t *rpm_feedback;
    char *modname;
    char *port;
} haldata_t;

typedef struct {
    bool running;
    bool extern_disable;
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

#define HAL_PIN_NEW(type, dir, var, comp_id, modname, name, initval) \
    do { \
        if (hal_pin_##type##_newf((dir), &(var), (comp_id), "%s." #name, (modname)) != 0) { \
            fprintf(stderr, "bmr_sfu: could not create " #name " pin\n"); \
            hal_exit((comp_id)); \
            return EXIT_FAILURE; \
        } \
        *(var) = (initval); \
    } while (0)

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
      /*.reserved =              (status & (1u << 0)) != 0,*/
        .running =               (status & (1u << 1)) != 0,
        .extern_disable =        (status & (1u << 2)) != 0,
      /*.remote_control_active = (status & (1u << 3)) != 0,
        .curr_speed_reached =    (status & (1u << 4)) != 0,*/
        .target_speed_reached =  (status & (1u << 5)) != 0,
        .stopped =               (status & (1u << 6)) != 0,
        .undervoltage =          (status & (1u << 7)) != 0,
        .overvoltage =           (status & (1u << 8)) != 0,
      /*.varioload_reached =     (status & (1u << 9)) != 0,*/
        .rs232_error =           (status & (1u << 10)) != 0,
        .spindle_not_ready =     (status & (1u << 11)) != 0,
        .converter_not_ready =   (status & (1u << 12)) != 0,
        .overload =              (status & (1u << 13)) != 0,
        .converter_overtemp =    (status & (1u << 14)) != 0,
        .spindle_overtemp =      (status & (1u << 15)) != 0
    };
    return s;
}

static int set_speed(double *current_rpm, double target_rpm) 
{
    uint16_t response_value;
    if (write_word(COMMAND_SET_SPEED, RESPONSE_SET_SPEED, (uint16_t)(fabs(target_rpm) / 10.0), &response_value) != 0) {
        fprintf(stderr, "bmr_sfu: set speed failed\n");
        return -1;
    }
    *current_rpm = target_rpm;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    struct tm tm;
    localtime_r(&now.tv_sec, &tm);

    printf("%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, now.tv_nsec / 1000000);
    printf("Set Speed: %u\n", (unsigned)response_value * 10u);
    return 0;
}

static int start_spindle(double *current_rpm, double target_rpm, bool dir_cw)
{
    uint16_t response_value;
    if (set_speed(current_rpm, target_rpm) != 0)
        return -1;

    if (dir_cw) {
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
    if (read_word(COMMAND_STOP, RESPONSE_STOP, &value) != 0) {
        fprintf(stderr, "bmr_sfu: stop failed\n");
        return -1;
    }

    printf("Stop: 0x%X\n", value);
    return 0;
}

static void set_comm_error(haldata_t *haldata, bool state)
{
    if (haldata != NULL && haldata->comm_error != NULL)
        *(haldata->comm_error) = state;
    if (state)
        fprintf(stderr, "bmr_sfu151: rs232 communication error\n");
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

    HAL_PIN_NEW(bit, HAL_IN, haldata->spindle_start, comp_id, modname, start, 0);
    HAL_PIN_NEW(bit, HAL_IN, haldata->spindle_cw, comp_id, modname, spindle-cw, 1);
    HAL_PIN_NEW(bit, HAL_IN, haldata->spindle_ccw, comp_id, modname, spindle-ccw, 0);
    HAL_PIN_NEW(float, HAL_IN, haldata->spindle_rpm, comp_id, modname, spindle-rpm, 5000.0);

    HAL_PIN_NEW(float, HAL_IN, haldata->loop_time, comp_id, modname, loop-time, 0.1);
    HAL_PIN_NEW(float, HAL_IN, haldata->ramp_rpm, comp_id, modname, ramp-rpm, 5000.0);
    HAL_PIN_NEW(float, HAL_IN, haldata->min_rpm, comp_id, modname, min-rpm, 2000.0);


    HAL_PIN_NEW(bit, HAL_OUT, haldata->running, comp_id, modname, status.running, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->target_speed_reached, comp_id, modname, status.target-speed-reached, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->stopped, comp_id, modname, status.stopped, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->undervoltage, comp_id, modname, status.undervoltage, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->overvoltage, comp_id, modname, status.overvoltage, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->rs232_error, comp_id, modname, status.rs232-error, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->spindle_not_ready, comp_id, modname, status.spindle-not-ready, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->converter_not_ready, comp_id, modname, status.converter-not-ready, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->overload, comp_id, modname, status.overload, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->converter_overtemp, comp_id, modname, status.converter-overtemp, 0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->spindle_overtemp, comp_id, modname, status.spindle-overtemp, 0);
    HAL_PIN_NEW(u32, HAL_OUT, haldata->status_word, comp_id, modname, status_word, 0);
    HAL_PIN_NEW(float, HAL_OUT, haldata->current, comp_id, modname, current, 0.0);
    HAL_PIN_NEW(float, HAL_OUT, haldata->voltage, comp_id, modname, voltage, 0.0);
    HAL_PIN_NEW(float, HAL_OUT, haldata->rpm_feedback, comp_id, modname, rpm-feedback, 0.0);
    HAL_PIN_NEW(bit, HAL_OUT, haldata->comm_error, comp_id, modname, status.comm-error, 0);

    hal_ready(comp_id);

    serial_fd = configure_serial(port);
    if (serial_fd < 0) {
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }

    printf("Connected to %s @ 115200 baud\n", port);
    
    // For input pins
    bool spindle_start;
    bool spindle_cw;
    bool spindle_ccw;
    double spindle_rpm;
    double min_rpm;
    double ramp_step_rpm;

    // For output pins
    uint16_t status_word;
    uint16_t current_raw;
    uint16_t voltage_raw;
    uint16_t speed_raw;
    
    SpindleStatus status;
    bool spindle_dir_cw = true;
    bool spindle_start_last = false;
    bool spindle_cw_last = true;
    bool spindle_ccw_last = false;
    bool request_start = false;
    bool request_stop = false;
    bool spindle_running = false;
    double current_rpm = 0;
    double commanded_rpm = 0;
    double target_rpm;

    struct timespec now;
    struct timespec next;
    long period_ns;

    while (!done) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        next = now;
        period_ns = (long)(*(haldata->loop_time) * 1000000000.0);

        next.tv_sec += (time_t)(period_ns / 1000000000L);
        next.tv_nsec += (long)(period_ns % 1000000000L);
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }

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
        spindle_rpm = fabs(*(haldata->spindle_rpm));
        min_rpm = *(haldata->min_rpm);
        ramp_step_rpm = *(haldata->ramp_rpm) * *(haldata->loop_time);

        // Read status data
        if (write_word(COMMAND_SET_DP, RESPONSE_SET_DP, ADDR_STATUS, &status_word) != 0) {
            set_comm_error(haldata, true);
            *(haldata->running) = 0;
            usleep(10000);
            continue;
        }
        set_comm_error(haldata, false);
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
        *(haldata->rpm_feedback) = speed_raw * (spindle_dir_cw?10.0:-10.0);

        // Start pin changed
        if (spindle_start != spindle_start_last) {
            spindle_start_last = spindle_start;
            if (spindle_start) {
                if (spindle_rpm > min_rpm) {
                    request_start = true;
                    request_stop = false;
                }
            } else {
                request_stop = true;
                request_start = false;
            }
        }
        
        // Direction changed
        if (spindle_cw != spindle_cw_last || spindle_ccw != spindle_ccw_last) {
            spindle_cw_last = spindle_cw;
            spindle_ccw_last = spindle_ccw;
            if (spindle_running && spindle_start) {
                request_stop = true;
                request_start = true;
            }
        }

        // The start command is only accepted when the spindle is stopped. So wait here for stop.
        if (request_start) {
            if (status.stopped) {
                if (spindle_cw && !spindle_ccw) spindle_dir_cw = true;
                else if (!spindle_cw && spindle_ccw) spindle_dir_cw = false;
                else continue;

                if (start_spindle(&current_rpm, min_rpm, spindle_dir_cw) != 0) {
                    fprintf(stderr, "bmr_sfu: start failed\n");
                    set_comm_error(haldata, true);
                    continue;
                }
                commanded_rpm = min_rpm;
                request_start = false;
                spindle_running = true;
            }
        }

        // Wait until ramped down to min_rpm before sending stop command
        if (request_stop && current_rpm <= min_rpm) {
            if (stop_spindle() != 0) {
                set_comm_error(haldata, true);
                continue;
            }
            request_stop = false;
            spindle_running = false;
        }

        // Ramp down to min_rpm if stop requested, otherwise take value from pin
        if (request_stop) {
            target_rpm = min_rpm;
        } else {
            target_rpm = spindle_rpm;
        }

        // Ramp spindle
        if (target_rpm != current_rpm && spindle_running) { // it is probably bad to compare floats
            if (target_rpm > current_rpm) {
                if(commanded_rpm + ramp_step_rpm < spindle_rpm){
                    commanded_rpm += ramp_step_rpm;
                } else {
                    commanded_rpm = spindle_rpm;
                } 
            } else {
                if (commanded_rpm - ramp_step_rpm > min_rpm)
                    commanded_rpm -= ramp_step_rpm;
                else {
                    commanded_rpm = min_rpm;
                }
            }
            if (set_speed(&current_rpm, commanded_rpm) == -1) {
                set_comm_error(haldata, true);
                continue;
            }
            commanded_rpm = current_rpm;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

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
