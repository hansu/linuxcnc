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
 * Linux C port of bmr_sfu151.py
 * BMR SFU 0151 VFD serial control, 115200 baud, 8N1.
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
    hal_bit_t *start;
    hal_bit_t *spindle_cw;
    hal_bit_t *spindle_ccw;
    hal_float_t *spindle_rpm;
    char *modname;
    char *port;
} haldata_t;

typedef struct {
    bool running;
    bool target_speed_reached;
    bool stopped;
    bool undervoltage;
    bool overvoltage;
    bool rs232_status;
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

static void get_timestamp(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(buf, size, "%H:%M:%S", &tm_now);
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int configure_serial(const char *port)
{
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", port, strerror(errno));
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
            fprintf(stderr, "serial write: %s\n", strerror(errno));
            return -1;
        }
        sent += (size_t)n;
    }

    /* Equivalent to pyserial's flush(): wait until bytes have left the driver. */
    if (tcdrain(serial_fd) != 0) {
        fprintf(stderr, "tcdrain: %s\n", strerror(errno));
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
        .rs232_status = (status & (1u << 10)) != 0,
        /* Python source's comment/table say bit 11; it accidentally tested bit 10. */
        .spindle_not_ready = (status & (1u << 11)) != 0,
        .converter_not_ready = (status & (1u << 12)) != 0,
        .overload = (status & (1u << 13)) != 0,
        .converter_overtemp = (status & (1u << 14)) != 0,
        .spindle_overtemp = (status & (1u << 15)) != 0
    };
    return s;
}

static int stop_spindle(void)
{
    uint16_t value;
    if (read_word(COMMAND_STOP, RESPONSE_STOP, &value) != 0)
        return -1;

    printf("Stop: 0x%X\n", value);
    return 0;
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

    if (hal_pin_bit_newf(HAL_IN, &(haldata->start), comp_id, "%s.start", modname) != 0) {
        fprintf(stderr, "bmr_sfu151: could not create start pin\n");
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }
    *haldata->start = 0;
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


    hal_ready(comp_id);

    serial_fd = configure_serial(port);
    if (serial_fd < 0) {
        hal_exit(comp_id);
        return EXIT_FAILURE;
    }

    char now[16];
    get_timestamp(now, sizeof(now));
    printf("[%s] Connected to %s @ 115200 baud\n", now, port);

    bool spindle_cw;
    bool spindle_ccw;
    double spindle_rpm;
    
    while (!done) {
        
        if (!*(haldata->start)) {
            usleep(1000);
            continue;
        }
        
        // Start spindle
        if (*(haldata->start)) {
            uint16_t response_value;
            spindle_cw = *(haldata->spindle_cw);
            spindle_ccw = *(haldata->spindle_ccw);
            spindle_rpm = *(haldata->spindle_rpm);
            
            if (write_word(COMMAND_SET_SPEED, RESPONSE_SET_SPEED, (uint16_t)(spindle_rpm / 10.0), &response_value) != 0)
                goto cleanup;
            printf("Set Speed: %u\n", (unsigned)response_value * 10u);

            if (spindle_cw) {
                if (write_word(COMMAND_SET_DIR_CW, RESPONSE_SET_DIR_CW, 0, &response_value) != 0)
                    goto cleanup;
            } else if (spindle_ccw) {
                if (write_word(COMMAND_SET_DIR_CCW, RESPONSE_SET_DIR_CCW, 0, &response_value) != 0)
                    goto cleanup;
            }

            if (read_word(COMMAND_START, RESPONSE_START, &response_value) != 0)
                goto cleanup;
            printf("Start: 0x%X\n", response_value);
        }

        // Keep alive loop
        while (*(haldata->start)){
            uint16_t status_word;
            uint16_t current_raw;
            uint16_t voltage_raw;
            uint16_t speed_raw;

            if (write_word(COMMAND_SET_DP, RESPONSE_SET_DP, ADDR_STATUS, &status_word) != 0)
                break;
            SpindleStatus status = get_status_bits(status_word);
            printf("Target speed reached: %s\n", status.target_speed_reached ? "true" : "false");

            if (write_word(COMMAND_SET_DP, RESPONSE_SET_DP, ADDR_CURRENT, &current_raw) != 0)
                break;

            if (write_word(COMMAND_SET_DP, RESPONSE_SET_DP, ADDR_VOLTAGE, &voltage_raw) != 0)
                break;

            if (read_word(COMMAND_GET_SPEED_OUTPUT, RESPONSE_GET_SPEED_OUTPUT, &speed_raw) != 0)
                break;

            double current = current_raw / 100.0;
            double voltage = voltage_raw / 10.0;
            unsigned rpm_feedback = (unsigned)speed_raw * 10u;

            printf("Speed: %u\n", rpm_feedback);
            printf("Current: %.2f\n", current);
            printf("Voltage: %.1f\n", voltage);
            printf("---------------\n");
            fflush(stdout);

            struct timespec req = { .tv_sec = 1, .tv_nsec = 0 };
            while (!done && nanosleep(&req, &req) != 0) {
                if (errno != EINTR)
                    break;
            }
        }
        stop_spindle();
    }

    exit_code = EXIT_SUCCESS;

cleanup:
    stop_spindle();

    if (done) {
        get_timestamp(now, sizeof(now));
        printf("\n[%s] Stopped\n", now);
    }

    if (serial_fd >= 0) {
        close(serial_fd);
        serial_fd = -1;
    }

    hal_exit(comp_id);
    return exit_code;
}
