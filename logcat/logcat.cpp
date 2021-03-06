// Copyright 2006-2015 The Android Open Source Project

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include <cutils/sockets.h>
#include <log/log.h>
#include <log/log_read.h>
#include <log/logger.h>
#include <log/logd.h>
#include <log/logprint.h>
#include <log/event_tag_map.h>

#define DEFAULT_LOG_ROTATE_SIZE_KBYTES 16
#define DEFAULT_MAX_ROTATED_LOGS 4

static AndroidLogFormat * g_logformat;

/* logd prefixes records with a length field */
#define RECORD_LENGTH_FIELD_SIZE_BYTES sizeof(uint32_t)

struct log_device_t {
    const char* device;
    bool binary;
    struct logger *logger;
    struct logger_list *logger_list;
    bool printed;

    log_device_t* next;

    log_device_t(const char* d, bool b) {
        device = d;
        binary = b;
        next = NULL;
        printed = false;
    }
};

namespace android {

/* Global Variables */

static const char * g_outputFileName = NULL;
static int g_logRotateSizeKBytes = 0;                   // 0 means "no log rotation"
static int g_maxRotatedLogs = DEFAULT_MAX_ROTATED_LOGS; // 0 means "unbounded"
static int g_outFD = -1;
static off_t g_outByteCount = 0;
static int g_printBinary = 0;
static int g_devCount = 0;                              // >1 means multiple

static int openLogFile (const char *pathname)
{
    return open(pathname, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
}

static void rotateLogs()
{
    int err;

    // Can't rotate logs if we're not outputting to a file
    if (g_outputFileName == NULL) {
        return;
    }

    close(g_outFD);

    // Compute the maximum number of digits needed to count up to g_maxRotatedLogs in decimal.
    // eg: g_maxRotatedLogs == 30 -> log10(30) == 1.477 -> maxRotationCountDigits == 2
    int maxRotationCountDigits =
            (g_maxRotatedLogs > 0) ? (int) (floor(log10(g_maxRotatedLogs) + 1)) : 0;

    for (int i = g_maxRotatedLogs ; i > 0 ; i--) {
        char *file0, *file1;

        asprintf(&file1, "%s.%.*d", g_outputFileName, maxRotationCountDigits, i);

        if (i - 1 == 0) {
            asprintf(&file0, "%s", g_outputFileName);
        } else {
            asprintf(&file0, "%s.%.*d", g_outputFileName, maxRotationCountDigits, i - 1);
        }

        err = rename (file0, file1);

        if (err < 0 && errno != ENOENT) {
            perror("while rotating log files");
        }

        free(file1);
        free(file0);
    }

    g_outFD = openLogFile (g_outputFileName);

    if (g_outFD < 0) {
        perror ("couldn't open output file");
        exit(-1);
    }

    g_outByteCount = 0;

}

void printBinary(struct log_msg *buf)
{
    size_t size = buf->len();

    TEMP_FAILURE_RETRY(write(g_outFD, buf, size));
}

static void processBuffer(log_device_t* dev, struct log_msg *buf)
{
    int bytesWritten = 0;
    int err;
    AndroidLogEntry entry;
    char binaryMsgBuf[1024];

    if (dev->binary) {
        static bool hasOpenedEventTagMap = false;
        static EventTagMap *eventTagMap = NULL;

        if (!eventTagMap && !hasOpenedEventTagMap) {
            eventTagMap = android_openEventTagMap(EVENT_TAG_MAP_FILE);
            hasOpenedEventTagMap = true;
        }
        err = android_log_processBinaryLogBuffer(&buf->entry_v1, &entry,
                                                 eventTagMap,
                                                 binaryMsgBuf,
                                                 sizeof(binaryMsgBuf));
        //printf(">>> pri=%d len=%d msg='%s'\n",
        //    entry.priority, entry.messageLen, entry.message);
    } else {
        err = android_log_processLogBuffer(&buf->entry_v1, &entry);
    }
    if (err < 0) {
        goto error;
    }

    if (android_log_shouldPrintLine(g_logformat, entry.tag, entry.priority)) {
        bytesWritten = android_log_printLogLine(g_logformat, g_outFD, &entry);

        if (bytesWritten < 0) {
            perror("output error");
            exit(-1);
        }
    }

    g_outByteCount += bytesWritten;

    if (g_logRotateSizeKBytes > 0
        && (g_outByteCount / 1024) >= g_logRotateSizeKBytes
    ) {
        rotateLogs();
    }

error:
    //fprintf (stderr, "Error processing record\n");
    return;
}

static void maybePrintStart(log_device_t* dev, bool printDividers) {
    if (!dev->printed || printDividers) {
        if (g_devCount > 1 && !g_printBinary) {
            char buf[1024];
            snprintf(buf, sizeof(buf), "--------- %s %s\n",
                     dev->printed ? "switch to" : "beginning of",
                     dev->device);
            if (write(g_outFD, buf, strlen(buf)) < 0) {
                perror("output error");
                exit(-1);
            }
        }
        dev->printed = true;
    }
}

static void setupOutput()
{

    if (g_outputFileName == NULL) {
        g_outFD = STDOUT_FILENO;

    } else {
        struct stat statbuf;

        g_outFD = openLogFile (g_outputFileName);

        if (g_outFD < 0) {
            perror ("couldn't open output file");
            exit(-1);
        }

        fstat(g_outFD, &statbuf);

        g_outByteCount = statbuf.st_size;
    }
}

static void show_help(const char *cmd)
{
    fprintf(stderr,"Usage: %s [options] [filterspecs]\n", cmd);

    fprintf(stderr, "options include:\n"
                    "  -s              Set default filter to silent.\n"
                    "                  Like specifying filterspec '*:S'\n"
                    "  -f <filename>   Log to file. Default to stdout\n"
                    "  -r [<kbytes>]   Rotate log every kbytes. (16 if unspecified). Requires -f\n"
                    "  -n <count>      Sets max number of rotated logs to <count>, default 4\n"
                    "  -v <format>     Sets the log print format, where <format> is:\n\n"
                    "                  brief color long process raw tag thread threadtime time\n\n"
                    "  -D              print dividers between each log buffer\n"
                    "  -c              clear (flush) the entire log and exit\n"
                    "  -d              dump the log and then exit (don't block)\n"
                    "  -t <count>      print only the most recent <count> lines (implies -d)\n"
                    "  -t '<time>'     print most recent lines since specified time (implies -d)\n"
                    "  -T <count>      print only the most recent <count> lines (does not imply -d)\n"
                    "  -T '<time>'     print most recent lines since specified time (not imply -d)\n"
                    "                  count is pure numerical, time is 'MM-DD hh:mm:ss.mmm'\n"
                    "  -g              get the size of the log's ring buffer and exit\n"
                    "  -L              dump logs from prior to last reboot\n"
                    "  -b <buffer>     Request alternate ring buffer, 'main', 'system', 'radio',\n"
                    "                  'events', 'crash' or 'all'. Multiple -b parameters are\n"
                    "                  allowed and results are interleaved. The default is\n"
                    "                  -b main -b system -b crash.\n"
                    "  -B              output the log in binary.\n"
                    "  -S              output statistics.\n"
                    "  -G <size>       set size of log ring buffer, may suffix with K or M.\n"
                    "  -p              print prune white and ~black list. Service is specified as\n"
                    "                  UID, UID/PID or /PID. Weighed for quicker pruning if prefix\n"
                    "                  with ~, otherwise weighed for longevity if unadorned. All\n"
                    "                  other pruning activity is oldest first. Special case ~!\n"
                    "                  represents an automatic quicker pruning for the noisiest\n"
                    "                  UID as determined by the current statistics.\n"
                    "  -P '<list> ...' set prune white and ~black list, using same format as\n"
                    "                  printed above. Must be quoted.\n");

    fprintf(stderr,"\nfilterspecs are a series of \n"
                   "  <tag>[:priority]\n\n"
                   "where <tag> is a log component tag (or * for all) and priority is:\n"
                   "  V    Verbose (default for <tag>)\n"
                   "  D    Debug (default for '*')\n"
                   "  I    Info\n"
                   "  W    Warn\n"
                   "  E    Error\n"
                   "  F    Fatal\n"
                   "  S    Silent (suppress all output)\n"
                   "\n'*' by itself means '*:D' and <tag> by itself means <tag>:V.\n"
                   "If no '*' filterspec or -s on command line, all filter defaults to '*:V'.\n"
                   "eg: '*:S <tag>' prints only <tag>, '<tag>:S' suppresses all <tag> log messages.\n"
                   "\nIf not specified on the command line, filterspec is set from ANDROID_LOG_TAGS.\n"
                   "\nIf not specified with -v on command line, format is set from ANDROID_PRINTF_LOG\n"
                   "or defaults to \"threadtime\"\n\n");
}


} /* namespace android */

static int setLogFormat(const char * formatString)
{
    static AndroidLogPrintFormat format;

    format = android_log_formatFromString(formatString);

    if (format == FORMAT_OFF) {
        // FORMAT_OFF means invalid string
        return -1;
    }

    android_log_setPrintFormat(g_logformat, format);

    return 0;
}

static const char multipliers[][2] = {
    { "" },
    { "K" },
    { "M" },
    { "G" }
};

static unsigned long value_of_size(unsigned long value)
{
    for (unsigned i = 0;
            (i < sizeof(multipliers)/sizeof(multipliers[0])) && (value >= 1024);
            value /= 1024, ++i) ;
    return value;
}

static const char *multiplier_of_size(unsigned long value)
{
    unsigned i;
    for (i = 0;
            (i < sizeof(multipliers)/sizeof(multipliers[0])) && (value >= 1024);
            value /= 1024, ++i) ;
    return multipliers[i];
}

int main(int argc, char **argv)
{
    int err;
    int hasSetLogFormat = 0;
    int clearLog = 0;
    int getLogSize = 0;
    unsigned long setLogSize = 0;
    int getPruneList = 0;
    char *setPruneList = NULL;
    int printStatistics = 0;
    int mode = ANDROID_LOG_RDONLY;
    const char *forceFilters = NULL;
    log_device_t* devices = NULL;
    log_device_t* dev;
    bool printDividers = false;
    struct logger_list *logger_list;
    unsigned int tail_lines = 0;
    log_time tail_time(log_time::EPOCH);

    signal(SIGPIPE, exit);

    g_logformat = android_log_format_new();

    if (argc == 2 && 0 == strcmp(argv[1], "--help")) {
        android::show_help(argv[0]);
        exit(0);
    }

    for (;;) {
        int ret;

        ret = getopt(argc, argv, "cdDLt:T:gG:sQf:r:n:v:b:BSpP:");

        if (ret < 0) {
            break;
        }

        switch(ret) {
            case 's':
                // default to all silent
                android_log_addFilterRule(g_logformat, "*:s");
            break;

            case 'c':
                clearLog = 1;
                mode |= ANDROID_LOG_WRONLY;
            break;

            case 'L':
                mode |= ANDROID_LOG_PSTORE;
            break;

            case 'd':
                mode |= ANDROID_LOG_RDONLY | ANDROID_LOG_NONBLOCK;
            break;

            case 't':
                mode |= ANDROID_LOG_RDONLY | ANDROID_LOG_NONBLOCK;
                /* FALLTHRU */
            case 'T':
                if (strspn(optarg, "0123456789") != strlen(optarg)) {
                    char *cp = tail_time.strptime(optarg,
                                                  log_time::default_format);
                    if (!cp) {
                        fprintf(stderr,
                                "ERROR: -%c \"%s\" not in \"%s\" time format\n",
                                ret, optarg, log_time::default_format);
                        exit(1);
                    }
                    if (*cp) {
                        char c = *cp;
                        *cp = '\0';
                        fprintf(stderr,
                                "WARNING: -%c \"%s\"\"%c%s\" time truncated\n",
                                ret, optarg, c, cp + 1);
                        *cp = c;
                    }
                } else {
                    tail_lines = atoi(optarg);
                    if (!tail_lines) {
                        fprintf(stderr,
                                "WARNING: -%c %s invalid, setting to 1\n",
                                ret, optarg);
                        tail_lines = 1;
                    }
                }
            break;

            case 'D':
                printDividers = true;
            break;

            case 'g':
                getLogSize = 1;
            break;

            case 'G': {
                // would use atol if not for the multiplier
                char *cp = optarg;
                setLogSize = 0;
                while (('0' <= *cp) && (*cp <= '9')) {
                    setLogSize *= 10;
                    setLogSize += *cp - '0';
                    ++cp;
                }

                switch(*cp) {
                case 'g':
                case 'G':
                    setLogSize *= 1024;
                /* FALLTHRU */
                case 'm':
                case 'M':
                    setLogSize *= 1024;
                /* FALLTHRU */
                case 'k':
                case 'K':
                    setLogSize *= 1024;
                /* FALLTHRU */
                case '\0':
                break;

                default:
                    setLogSize = 0;
                }

                if (!setLogSize) {
                    fprintf(stderr, "ERROR: -G <num><multiplier>\n");
                    exit(1);
                }
            }
            break;

            case 'p':
                getPruneList = 1;
            break;

            case 'P':
                setPruneList = optarg;
            break;

            case 'b': {
                if (strcmp(optarg, "all") == 0) {
                    while (devices) {
                        dev = devices;
                        devices = dev->next;
                        delete dev;
                    }

                    devices = dev = NULL;
                    android::g_devCount = 0;
                    for(int i = LOG_ID_MIN; i < LOG_ID_MAX; ++i) {
                        const char *name = android_log_id_to_name((log_id_t)i);
                        log_id_t log_id = android_name_to_log_id(name);

                        if (log_id != (log_id_t)i) {
                            continue;
                        }

                        bool binary = strcmp(name, "events") == 0;
                        log_device_t* d = new log_device_t(name, binary);

                        if (dev) {
                            dev->next = d;
                            dev = d;
                        } else {
                            devices = dev = d;
                        }
                        android::g_devCount++;
                    }
                    break;
                }

                bool binary = strcmp(optarg, "events") == 0;

                if (devices) {
                    dev = devices;
                    while (dev->next) {
                        dev = dev->next;
                    }
                    dev->next = new log_device_t(optarg, binary);
                } else {
                    devices = new log_device_t(optarg, binary);
                }
                android::g_devCount++;
            }
            break;

            case 'B':
                android::g_printBinary = 1;
            break;

            case 'f':
                // redirect output to a file

                android::g_outputFileName = optarg;

            break;

            case 'r':
                if (optarg == NULL) {
                    android::g_logRotateSizeKBytes
                                = DEFAULT_LOG_ROTATE_SIZE_KBYTES;
                } else {
                    if (!isdigit(optarg[0])) {
                        fprintf(stderr,"Invalid parameter to -r\n");
                        android::show_help(argv[0]);
                        exit(-1);
                    }
                    android::g_logRotateSizeKBytes = atoi(optarg);
                }
            break;

            case 'n':
                if (!isdigit(optarg[0])) {
                    fprintf(stderr,"Invalid parameter to -r\n");
                    android::show_help(argv[0]);
                    exit(-1);
                }

                android::g_maxRotatedLogs = atoi(optarg);
            break;

            case 'v':
                err = setLogFormat (optarg);
                if (err < 0) {
                    fprintf(stderr,"Invalid parameter to -v\n");
                    android::show_help(argv[0]);
                    exit(-1);
                }

                if (strcmp("color", optarg)) { // exception for modifiers
                    hasSetLogFormat = 1;
                }
            break;

            case 'Q':
                /* this is a *hidden* option used to start a version of logcat                 */
                /* in an emulated device only. it basically looks for androidboot.logcat=      */
                /* on the kernel command line. If something is found, it extracts a log filter */
                /* and uses it to run the program. If nothing is found, the program should     */
                /* quit immediately                                                            */
#define  KERNEL_OPTION  "androidboot.logcat="
#define  CONSOLE_OPTION "androidboot.console="
                {
                    int          fd;
                    char*        logcat;
                    char*        console;
                    int          force_exit = 1;
                    static char  cmdline[1024];

                    fd = open("/proc/cmdline", O_RDONLY);
                    if (fd >= 0) {
                        int  n = read(fd, cmdline, sizeof(cmdline)-1 );
                        if (n < 0) n = 0;
                        cmdline[n] = 0;
                        close(fd);
                    } else {
                        cmdline[0] = 0;
                    }

                    logcat  = strstr( cmdline, KERNEL_OPTION );
                    console = strstr( cmdline, CONSOLE_OPTION );
                    if (logcat != NULL) {
                        char*  p = logcat + sizeof(KERNEL_OPTION)-1;;
                        char*  q = strpbrk( p, " \t\n\r" );;

                        if (q != NULL)
                            *q = 0;

                        forceFilters = p;
                        force_exit   = 0;
                    }
                    /* if nothing found or invalid filters, exit quietly */
                    if (force_exit)
                        exit(0);

                    /* redirect our output to the emulator console */
                    if (console) {
                        char*  p = console + sizeof(CONSOLE_OPTION)-1;
                        char*  q = strpbrk( p, " \t\n\r" );
                        char   devname[64];
                        int    len;

                        if (q != NULL) {
                            len = q - p;
                        } else
                            len = strlen(p);

                        len = snprintf( devname, sizeof(devname), "/dev/%.*s", len, p );
                        fprintf(stderr, "logcat using %s (%d)\n", devname, len);
                        if (len < (int)sizeof(devname)) {
                            fd = open( devname, O_WRONLY );
                            if (fd >= 0) {
                                dup2(fd, 1);
                                dup2(fd, 2);
                                close(fd);
                            }
                        }
                    }
                }
                break;

            case 'S':
                printStatistics = 1;
                break;

            default:
                fprintf(stderr,"Unrecognized Option\n");
                android::show_help(argv[0]);
                exit(-1);
            break;
        }
    }

    if (!devices) {
        dev = devices = new log_device_t("main", false);
        android::g_devCount = 1;
        if (android_name_to_log_id("system") == LOG_ID_SYSTEM) {
            dev = dev->next = new log_device_t("system", false);
            android::g_devCount++;
        }
        if (android_name_to_log_id("crash") == LOG_ID_CRASH) {
            dev = dev->next = new log_device_t("crash", false);
            android::g_devCount++;
        }
    }

    if (android::g_logRotateSizeKBytes != 0
        && android::g_outputFileName == NULL
    ) {
        fprintf(stderr,"-r requires -f as well\n");
        android::show_help(argv[0]);
        exit(-1);
    }

    android::setupOutput();

    if (hasSetLogFormat == 0) {
        const char* logFormat = getenv("ANDROID_PRINTF_LOG");

        if (logFormat != NULL) {
            err = setLogFormat(logFormat);
            if (err < 0) {
                fprintf(stderr, "invalid format in ANDROID_PRINTF_LOG '%s'\n",
                                    logFormat);
            }
        } else {
            setLogFormat("threadtime");
        }
    }

    if (forceFilters) {
        err = android_log_addFilterString(g_logformat, forceFilters);
        if (err < 0) {
            fprintf (stderr, "Invalid filter expression in -logcat option\n");
            exit(0);
        }
    } else if (argc == optind) {
        // Add from environment variable
        char *env_tags_orig = getenv("ANDROID_LOG_TAGS");

        if (env_tags_orig != NULL) {
            err = android_log_addFilterString(g_logformat, env_tags_orig);

            if (err < 0) {
                fprintf(stderr, "Invalid filter expression in"
                                    " ANDROID_LOG_TAGS\n");
                android::show_help(argv[0]);
                exit(-1);
            }
        }
    } else {
        // Add from commandline
        for (int i = optind ; i < argc ; i++) {
            err = android_log_addFilterString(g_logformat, argv[i]);

            if (err < 0) {
                fprintf (stderr, "Invalid filter expression '%s'\n", argv[i]);
                android::show_help(argv[0]);
                exit(-1);
            }
        }
    }

    dev = devices;
    if (tail_time != log_time::EPOCH) {
        logger_list = android_logger_list_alloc_time(mode, tail_time, 0);
    } else {
        logger_list = android_logger_list_alloc(mode, tail_lines, 0);
    }
    while (dev) {
        dev->logger_list = logger_list;
        dev->logger = android_logger_open(logger_list,
                                          android_name_to_log_id(dev->device));
        if (!dev->logger) {
            fprintf(stderr, "Unable to open log device '%s'\n", dev->device);
            exit(EXIT_FAILURE);
        }

        if (clearLog) {
            int ret;
            ret = android_logger_clear(dev->logger);
            if (ret) {
                perror("failed to clear the log");
                exit(EXIT_FAILURE);
            }
        }

        if (setLogSize && android_logger_set_log_size(dev->logger, setLogSize)) {
            perror("failed to set the log size");
            exit(EXIT_FAILURE);
        }

        if (getLogSize) {
            long size, readable;

            size = android_logger_get_log_size(dev->logger);
            if (size < 0) {
                perror("failed to get the log size");
                exit(EXIT_FAILURE);
            }

            readable = android_logger_get_log_readable_size(dev->logger);
            if (readable < 0) {
                perror("failed to get the readable log size");
                exit(EXIT_FAILURE);
            }

            printf("%s: ring buffer is %ld%sb (%ld%sb consumed), "
                   "max entry is %db, max payload is %db\n", dev->device,
                   value_of_size(size), multiplier_of_size(size),
                   value_of_size(readable), multiplier_of_size(readable),
                   (int) LOGGER_ENTRY_MAX_LEN, (int) LOGGER_ENTRY_MAX_PAYLOAD);
        }

        dev = dev->next;
    }

    if (setPruneList) {
        size_t len = strlen(setPruneList) + 32; // margin to allow rc
        char *buf = (char *) malloc(len);

        strcpy(buf, setPruneList);
        int ret = android_logger_set_prune_list(logger_list, buf, len);
        free(buf);

        if (ret) {
            perror("failed to set the prune list");
            exit(EXIT_FAILURE);
        }
    }

    if (printStatistics || getPruneList) {
        size_t len = 8192;
        char *buf;

        for(int retry = 32;
                (retry >= 0) && ((buf = new char [len]));
                delete [] buf, --retry) {
            if (getPruneList) {
                android_logger_get_prune_list(logger_list, buf, len);
            } else {
                android_logger_get_statistics(logger_list, buf, len);
            }
            buf[len-1] = '\0';
            size_t ret = atol(buf) + 1;
            if (ret < 4) {
                delete [] buf;
                buf = NULL;
                break;
            }
            bool check = ret <= len;
            len = ret;
            if (check) {
                break;
            }
        }

        if (!buf) {
            perror("failed to read data");
            exit(EXIT_FAILURE);
        }

        // remove trailing FF
        char *cp = buf + len - 1;
        *cp = '\0';
        bool truncated = *--cp != '\f';
        if (!truncated) {
            *cp = '\0';
        }

        // squash out the byte count
        cp = buf;
        if (!truncated) {
            while (isdigit(*cp)) {
                ++cp;
            }
            if (*cp == '\n') {
                ++cp;
            }
        }

        printf("%s", cp);
        delete [] buf;
        exit(0);
    }


    if (getLogSize) {
        exit(0);
    }
    if (setLogSize || setPruneList) {
        exit(0);
    }
    if (clearLog) {
        exit(0);
    }

    //LOG_EVENT_INT(10, 12345);
    //LOG_EVENT_LONG(11, 0x1122334455667788LL);
    //LOG_EVENT_STRING(0, "whassup, doc?");

    dev = NULL;
    log_device_t unexpected("unexpected", false);
    while (1) {
        struct log_msg log_msg;
        log_device_t* d;
        int ret = android_logger_list_read(logger_list, &log_msg);

        if (ret == 0) {
            fprintf(stderr, "read: unexpected EOF!\n");
            exit(EXIT_FAILURE);
        }

        if (ret < 0) {
            if (ret == -EAGAIN) {
                break;
            }

            if (ret == -EIO) {
                fprintf(stderr, "read: unexpected EOF!\n");
                exit(EXIT_FAILURE);
            }
            if (ret == -EINVAL) {
                fprintf(stderr, "read: unexpected length.\n");
                exit(EXIT_FAILURE);
            }
            perror("logcat read failure");
            exit(EXIT_FAILURE);
        }

        for(d = devices; d; d = d->next) {
            if (android_name_to_log_id(d->device) == log_msg.id()) {
                break;
            }
        }
        if (!d) {
            android::g_devCount = 2; // set to Multiple
            d = &unexpected;
            d->binary = log_msg.id() == LOG_ID_EVENTS;
        }

        if (dev != d) {
            dev = d;
            android::maybePrintStart(dev, printDividers);
        }
        if (android::g_printBinary) {
            android::printBinary(&log_msg);
        } else {
            android::processBuffer(dev, &log_msg);
        }
    }

    android_logger_list_free(logger_list);

    return 0;
}
