#include <stddef.h>

#if defined(__APPLE__) && defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x1000000
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x20
#endif

static inline size_t bins_strnlen(const char *text, size_t limit) {
    size_t length = 0;
    while (length < limit && text[length]) ++length;
    return length;
}
#define strnlen bins_strnlen

/* Manifest tools are single-threaded; a held directory fd remains the parent
 * while each relative operation temporarily changes the working directory. */
static inline int bins_enter_dir(int dir) {
    int previous = open(".", O_RDONLY | O_DIRECTORY);
    if (previous < 0) return -1;
    if (fchdir(dir) < 0) {
        int saved = errno;
        close(previous);
        errno = saved;
        return -1;
    }
    return previous;
}
static inline int bins_leave_dir(int previous, int result) {
    int saved = errno;
    if (fchdir(previous) < 0) {
        saved = errno;
        result = -1;
    }
    close(previous);
    errno = saved;
    return result;
}
static inline int bins_openat(int dir, const char *name, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    int previous = bins_enter_dir(dir);
    if (previous < 0) return -1;
    int result = open(name, flags & ~O_CLOEXEC, mode);
    if (result >= 0 && (flags & O_CLOEXEC) && fcntl(result, F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(result);
        errno = saved;
        result = -1;
    }
    return bins_leave_dir(previous, result);
}
static inline int bins_mkdirat(int dir, const char *name, mode_t mode) {
    int previous = bins_enter_dir(dir);
    if (previous < 0) return -1;
    return bins_leave_dir(previous, mkdir(name, mode));
}
static inline int bins_symlinkat(const char *target, int dir, const char *name) {
    int previous = bins_enter_dir(dir);
    if (previous < 0) return -1;
    return bins_leave_dir(previous, symlink(target, name));
}
static inline ssize_t bins_readlinkat(int dir, const char *name, char *target, size_t size) {
    int previous = bins_enter_dir(dir);
    if (previous < 0) return -1;
    ssize_t result = readlink(name, target, size);
    int saved = errno;
    if (fchdir(previous) < 0) result = -1;
    else errno = saved;
    close(previous);
    return result;
}
static inline int bins_fstatat(int dir, const char *name, struct stat *info, int flags) {
    if (flags != AT_SYMLINK_NOFOLLOW) { errno = EINVAL; return -1; }
    int previous = bins_enter_dir(dir);
    if (previous < 0) return -1;
    return bins_leave_dir(previous, lstat(name, info));
}
static inline int bins_linkat(int source_dir, const char *source, int target_dir,
                              const char *target, int flags) {
    char directory[PATH_MAX], path[PATH_MAX];
    if (flags || fcntl(source_dir, F_GETPATH, directory) < 0 ||
        snprintf(path, sizeof(path), "%s/%s", directory, source) >= (int)sizeof(path)) {
        errno = EINVAL;
        return -1;
    }
    int previous = bins_enter_dir(target_dir);
    if (previous < 0) return -1;
    return bins_leave_dir(previous, link(path, target));
}
static inline ssize_t bins_getline(char **line, size_t *capacity, FILE *input) {
    if (!line || !capacity || !input) { errno = EINVAL; return -1; }
    size_t length = 0;
    int ch;
    while ((ch = fgetc(input)) != EOF) {
        if (length + 1 >= *capacity) {
            size_t size = *capacity ? *capacity * 2 : 128;
            if (size <= length + 1) size = length + 2;
            char *next = realloc(*line, size);
            if (!next) return -1;
            *line = next;
            *capacity = size;
        }
        (*line)[length++] = (char)ch;
        if (ch == '\n') break;
    }
    if (!length) return -1;
    (*line)[length] = 0;
    return (ssize_t)length;
}
#define openat bins_openat
#define mkdirat bins_mkdirat
#define symlinkat bins_symlinkat
#define readlinkat bins_readlinkat
#define fstatat bins_fstatat
#define linkat bins_linkat
#define getline bins_getline
#endif
