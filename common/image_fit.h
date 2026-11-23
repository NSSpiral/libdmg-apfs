/* POSIX memory-backed image planning. Never publishes a failed candidate. */
#ifndef IMAGE_FIT_H
#define IMAGE_FIT_H
#include "common.h"
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#define FIT_MIB UINT64_C(1048576)
#define FIT_LIMIT (UINT64_C(2) * 1024 * 1024 * 1024)
typedef struct {
    uint8_t *data;
    uint64_t size;
} fit_memory;
static int fit_read(io_func *io, off_t at, size_t size, void *data) {
    fit_memory *m = io->data;
    if (at < 0 || (uint64_t)at > m->size || size > m->size - (uint64_t)at)
        return 0;
    if (size)
        memcpy(data, m->data + (size_t)at, size);
    return 1;
}
static int fit_write(io_func *io, off_t at, size_t size, void *data) {
    fit_memory *m = io->data;
    if (at < 0 || (uint64_t)at > m->size || size > m->size - (uint64_t)at)
        return 0;
    if (size)
        memcpy(m->data + (size_t)at, data, size);
    return 1;
}
static void fit_close(io_func *io) {
    free(io->data);
    free(io);
}
static io_func *fit_io(void *data, uint64_t size) {
    io_func *io = calloc(1, sizeof(*io));
    fit_memory *m = calloc(1, sizeof(*m));
    if (!io || !m) {
        free(io);
        free(m);
        return NULL;
    }
    m->data = data;
    m->size = size;
    io->data = m;
    io->read = fit_read;
    io->write = fit_write;
    io->close = fit_close;
    return io;
}
static int fit_size(const char *text, uint64_t *out) {
    char *end;
    uint64_t n, m = 1;
    if (!text || *text < '0' || *text > '9')
        return 0;
    errno = 0;
    n = strtoull(text, &end, 10);
    if (errno)
        return 0;
    if (*end) {
        switch (*end++) {
        case 'k':
        case 'K':
            m = 1024;
            break;
        case 'm':
        case 'M':
            m = FIT_MIB;
            break;
        case 'g':
        case 'G':
            m = FIT_MIB * 1024;
            break;
        default:
            return 0;
        }
        if (*end)
            return 0;
    }
    if (n > INT64_MAX / m)
        return 0;
    *out = n * m;
    return 1;
}
typedef struct {
    uint64_t reserve, exact, maximum;
    unsigned seen;
} fit_options;
/* Returns zero for a tool-specific option, one for a parsed sizing option,
 * and minus one for an invalid or repeated sizing option. */
static int fit_option(fit_options *options, const char *name, const char *value) {
    unsigned flag;
    uint64_t parsed;
    if (!strcmp(name, "--reserve"))
        flag = 1;
    else if (!strcmp(name, "--size"))
        flag = 2;
    else if (!strcmp(name, "--maximum"))
        flag = 4;
    else
        return 0;
    if ((options->seen & flag) || !fit_size(value, &parsed) || (flag != 1 && !parsed))
        return -1;
    options->seen |= flag;
    if (flag == 1)
        options->reserve = parsed;
    else if (flag == 2)
        options->exact = parsed;
    else
        options->maximum = parsed;
    return 1;
}
static int fit_options_valid(const fit_options *options, uint64_t floor) {
    uint64_t maximum = options->maximum - options->maximum % FIT_MIB;
    if (maximum < floor || options->maximum > FIT_LIMIT ||
        options->reserve > (options->exact ? options->exact : maximum) ||
        (options->exact &&
         (options->exact < floor || options->exact > maximum || options->exact % FIT_MIB))) {
        fprintf(stderr,
                "invalid image sizing bounds: sizes must fit within %" PRIu64
                " MiB..2 GiB, with whole-MiB --size and room for --reserve\n",
                floor / FIT_MIB);
        return 0;
    }
    return 1;
}
static char *fit_workspace(const char *prefix) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    char *path = malloc(strlen(tmp) + strlen(prefix) + 10);
    if (!path)
        return NULL;
    sprintf(path, "%s/%s-XXXXXX", tmp, prefix);
    if (!mkdtemp(path)) {
        free(path);
        return NULL;
    }
    return path;
}
static int fit_remove_tree(const char *path) {
    struct stat st;
    struct dirent *entry;
    DIR *dir;
    int failed = 0;
    if (lstat(path, &st))
        return 1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    dir = opendir(path);
    if (!dir)
        return 1;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char *child = malloc(strlen(path) + strlen(entry->d_name) + 2);
        if (!child) {
            failed = 1;
            break;
        }
        sprintf(child, "%s/%s", path, entry->d_name);
        failed |= fit_remove_tree(child);
        free(child);
    }
    closedir(dir);
    return rmdir(path) != 0 || failed;
}
typedef int (*fit_builder)(void *, uint64_t, void *, uint64_t *);
typedef struct {
    uint64_t free_bytes;
    int complete;
} fit_shared;
typedef struct {
    void *data;
    uint64_t size, free_bytes;
    unsigned probes;
} fit_image;
/* A child isolates the legacy HFS importer's fatal error paths. Source errors
 * are fatal; only an explicit ENOSPC exit is a reason to increase capacity. */
static int fit_probe(uint64_t size, fit_builder build, void *context, fit_image *out) {
    int pipes[2], status;
    pid_t child;
    ssize_t got;
    char errors[4096], drain[4096];
    size_t used = 0;
    void *data;
    fit_shared *shared;
    if (!size || size > FIT_LIMIT || size > SIZE_MAX) {
        fprintf(stderr, "image planning exceeds the 2 GiB memory-image limit\n");
        return 1;
    }
    data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (data == MAP_FAILED || shared == MAP_FAILED) {
        int saved_errno = errno;
        if (data != MAP_FAILED)
            munmap(data, size);
        if (shared != MAP_FAILED)
            munmap(shared, sizeof(*shared));
        errno = saved_errno;
        perror("allocate image planning memory");
        return 1;
    }
    if (pipe(pipes)) {
        int saved_errno = errno;
        munmap(data, size);
        munmap(shared, sizeof(*shared));
        errno = saved_errno;
        perror("open image planning pipe");
        return 1;
    }
    fflush(NULL);
    child = fork();
    if (child < 0) {
        int saved_errno = errno;
        close(pipes[0]);
        close(pipes[1]);
        munmap(data, size);
        munmap(shared, sizeof(*shared));
        errno = saved_errno;
        perror("fork image planner");
        return 1;
    }
    if (child == 0) {
        int nullfd = open("/dev/null", O_WRONLY), ret;
        close(pipes[0]);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]);
        if (nullfd >= 0) {
            dup2(nullfd, STDOUT_FILENO);
            close(nullfd);
        }
        ret = build(data, size, context, &shared->free_bytes);
        shared->complete = 1;
        fflush(NULL);
        _exit(ret == 0 ? 0 : ret == 2 ? 2 : 1);
    }
    close(pipes[1]);
    while ((got = read(pipes[0], drain, sizeof(drain))) != 0) {
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        size_t n = (size_t)got;
        if (n > sizeof(errors) - 1 - used)
            n = sizeof(errors) - 1 - used;
        memcpy(errors + used, drain, n);
        used += n;
    }
    close(pipes[0]);
    errors[used] = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            status = -1;
            break;
        }
    }
    out->probes++;
    if (status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && shared->complete) {
        if (out->data)
            munmap(out->data, out->size);
        out->data = data;
        out->size = size;
        out->free_bytes = shared->free_bytes;
        munmap(shared, sizeof(*shared));
        return 0;
    }
    munmap(data, size);
    munmap(shared, sizeof(*shared));
    if (status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 2)
        return 2;
    fprintf(stderr, "%s%s", errors, used ? "" : "image planning failed\n");
    return 1;
}
static int fit_plan(fit_builder build, void *context, uint64_t floor, uint64_t maximum, uint64_t exact,
                    uint64_t reserve, fit_image *out) {
    uint64_t lo = floor / FIT_MIB - 1, hi = exact ? exact : floor;
    int ret;
    if (!maximum || maximum > FIT_LIMIT)
        maximum = FIT_LIMIT;
    maximum -= maximum % FIT_MIB;
    if (floor % FIT_MIB || maximum < floor ||
        (exact && (exact < floor || exact > maximum || exact % FIT_MIB)))
        return 1;
    for (;;) {
        ret = fit_probe(hi, build, context, out);
        if (ret == 1)
            return 1;
        if (!ret && out->free_bytes >= reserve)
            break;
        if (exact || hi == maximum)
            return 2;
        lo = hi / FIT_MIB;
        hi = hi > maximum / 2 ? maximum : hi * 2;
    }
    hi /= FIT_MIB;
    if (!exact)
        while (hi - lo > 1) {
            uint64_t mid = (lo + hi) / 2;
            fit_image candidate = {0};
            ret = fit_probe(mid * FIT_MIB, build, context, &candidate);
            out->probes += candidate.probes;
            if (ret == 1)
                return 1;
            if (!ret && candidate.free_bytes >= reserve) {
                if (out->data)
                    munmap(out->data, out->size);
                out->data = candidate.data;
                out->size = candidate.size;
                out->free_bytes = candidate.free_bytes;
                hi = mid;
            } else {
                if (candidate.data)
                    munmap(candidate.data, candidate.size);
                lo = mid;
            }
        }
    return 0;
}
static int fit_same(const struct stat *a, const struct stat *b) {
    int same = a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
               a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime;
#ifdef __APPLE__
    return same && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return same && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}
static int fit_publish(const char *path, fit_image *image, const struct stat *original) {
    char *temp = malloc(strlen(path) + 24);
    int fd = -1, ok = 0, saved_errno = 0;
    struct stat now;
    uint64_t at = 0;
    if (!temp)
        return 1;
    sprintf(temp, "%s.partial-XXXXXX", path);
    fd = mkstemp(temp);
    if (fd < 0)
        goto done;
    if (ftruncate(fd, (off_t)image->size))
        goto done;
    while (at < image->size) {
        size_t n = image->size - at > FIT_MIB ? FIT_MIB : (size_t)(image->size - at), i;
        uint8_t *p = (uint8_t *)image->data + at;
        for (i = 0; i < n && !p[i]; i++)
            ;
        if (i < n) {
            size_t written = 0;
            while (written < n) {
                ssize_t w = pwrite(fd, p + written, n - written, (off_t)(at + written));
                if (w < 0 && errno == EINTR)
                    continue;
                if (w <= 0) {
                    if (!w)
                        errno = EIO;
                    goto done;
                }
                written += (size_t)w;
            }
        }
        at += n;
    }
    if (original) {
        struct stat target;
        if (fstat(fd, &target))
            goto done;
        if ((target.st_uid != original->st_uid || target.st_gid != original->st_gid) &&
            fchown(fd, original->st_uid, original->st_gid))
            goto done;
        if (fchmod(fd, original->st_mode & 07777))
            goto done;
    }
    if (fsync(fd))
        goto done;
    if (original) {
        if (lstat(path, &now) || !S_ISREG(now.st_mode) || !fit_same(original, &now)) {
            fprintf(stderr, "source changed during shrink\n");
            errno = ESTALE;
            goto done;
        }
        if (rename(temp, path))
            goto done;
    } else if (link(temp, path))
        goto done;
    ok = 1;
done:
    saved_errno = errno;
    if (fd >= 0) {
        close(fd);
        unlink(temp);
    }
    free(temp);
    if (!ok) {
        errno = saved_errno;
        perror("publish image");
    }
    return !ok;
}
static void fit_release(fit_image *image) {
    if (image->data)
        munmap(image->data, image->size);
    image->data = NULL;
}
static void fit_report(const fit_image *image) {
    printf("{\"method\":\"native memory allocation\",\"size_bytes\":%" PRIu64 ",\"free_bytes\":%" PRIu64
           ",\"memory_probes\":%u,\"disk_images_written\":1}\n",
           image->size, image->free_bytes, image->probes);
}
#endif
#endif
