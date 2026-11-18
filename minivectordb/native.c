#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#define HAVE_AVX2 1
#endif

typedef struct {
    PyObject *vectors;
    Py_ssize_t count;
    Py_ssize_t dimension;
    const float *data[];
} Scan;

typedef struct {
    Py_ssize_t slot;
    double distance;
} Hit;

static int decimal_digit(char value)
{
    return value >= '0' && value <= '9';
}

static double read_decimal(const char *start, char **end)
{
    static const double powers_of_ten[] = {
        1.0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
        1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
        1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
    };
    const char *cursor = start;
    if (*cursor == '+' || *cursor == '-')
        ++cursor;
    const char *digits = cursor;
    uint64_t coefficient = 0;
    int count = 0;
    while (decimal_digit(*cursor) && count < 15) {
        coefficient = coefficient * 10 + (*cursor++ - '0');
        ++count;
    }
    if (decimal_digit(*cursor))
        return strtod(start, end);
    int fractional = 0;
    if (*cursor == '.') {
        ++cursor;
        while (decimal_digit(*cursor) && count < 15) {
            coefficient = coefficient * 10 + (*cursor++ - '0');
            ++count;
            ++fractional;
        }
    }
    if (count == 0 || decimal_digit(*cursor) ||
        (digits[0] == '0' && (*cursor == 'x' || *cursor == 'X')))
        return strtod(start, end);
    int shift = -fractional;
    if (*cursor == 'e' || *cursor == 'E') {
        ++cursor;
        int negative = *cursor == '-';
        if (*cursor == '-' || *cursor == '+')
            ++cursor;
        if (!decimal_digit(*cursor))
            return strtod(start, end);
        int exponent = 0;
        while (decimal_digit(*cursor) && exponent <= 37)
            exponent = exponent * 10 + (*cursor++ - '0');
        if (decimal_digit(*cursor))
            return strtod(start, end);
        shift += negative ? -exponent : exponent;
        if (shift < -22 || shift > 22)
            return strtod(start, end);
    }
    *end = (char *)cursor;
    // Both the coefficient and powers 10^0 through 10^22 fit exactly in a
    // double, so one operation suffices to round ordinary decimal input.
    double magnitude = shift >= 0 ? (double)coefficient * powers_of_ten[shift]
                                  : (double)coefficient / powers_of_ten[-shift];
    return *start == '-' ? -magnitude : magnitude;
}

static PyObject *read_vector(const char *cursor, char terminator, Py_ssize_t dimension,
                             const char **next)
{
    while (isspace((unsigned char)*cursor))
        ++cursor;
    if (*cursor++ != '[')
        return NULL;

    PyObject *result = PyBytes_FromStringAndSize(NULL, dimension * sizeof(float));
    if (result == NULL)
        return NULL;
    float *values = (float *)PyBytes_AS_STRING(result);
    for (Py_ssize_t i = 0; i < dimension; ++i) {
        while (isspace((unsigned char)*cursor))
            ++cursor;
        const char *number = cursor;
        if (*number == '+' || *number == '-')
            ++number;
        if (!isdigit((unsigned char)*number) && *number != '.')
            goto fallback;
        if (number[0] == '0' && (number[1] == 'x' || number[1] == 'X'))
            goto fallback;
        char *end;
        double value = read_decimal(cursor, &end);
        if (end == cursor || !isfinite(value) || fabs(value) > (double)FLT_MAX)
            goto fallback;
        values[i] = (float)value;
        cursor = end;
        while (isspace((unsigned char)*cursor))
            ++cursor;
        if (*cursor++ != (i + 1 == dimension ? ']' : ','))
            goto fallback;
    }
    while (isspace((unsigned char)*cursor))
        ++cursor;
    if (*cursor == terminator) {
        *next = cursor + 1;
        return result;
    }

fallback:
    Py_DECREF(result);
    return NULL;
}

static PyObject *parse_vector(PyObject *self, PyObject *args)
{
    const char *cursor, *next;
    Py_ssize_t dimension;
    if (!PyArg_ParseTuple(args, "sn", &cursor, &dimension))
        return NULL;
    if (dimension <= 0 || dimension > PY_SSIZE_T_MAX / (Py_ssize_t)sizeof(float))
        Py_RETURN_NONE;
    PyObject *result = read_vector(cursor, '\0', dimension, &next);
    if (result != NULL || PyErr_Occurred())
        return result;
    Py_RETURN_NONE;
}

static const char *skip_sql_space(const char *cursor)
{
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
        ++cursor;
    return cursor;
}

// The Python caller supplies a schema and the offset of VALUES. Only plain
// literals are accepted here. Any other form is handled by the SQL parser.
static PyObject *read_sql_literal(const char **position, const char *end, Py_ssize_t type)
{
    const char *cursor = skip_sql_space(*position);
    if (type == 0) {
        const char *digits = *cursor == '-' ? cursor + 1 : cursor;
        if (!decimal_digit(*digits))
            return NULL;
        char *next;
        errno = 0;
        long long integer = strtoll(cursor, &next, 10);
        if (errno == ERANGE || next - digits > 19)
            return NULL;
        *position = next;
        return PyLong_FromLongLong(integer);
    }
    char quote = *cursor;
    if (quote != '\'' && quote != '"')
        return NULL;
    ++cursor;
    if (type > 0) {
        if (type > end - cursor || type > PY_SSIZE_T_MAX / (Py_ssize_t)sizeof(float))
            return NULL;
        return read_vector(cursor, quote, type, position);
    }
    const char *close = strchr(cursor, quote);
    if (close == NULL || memchr(cursor, '\\', close - cursor) != NULL)
        return NULL;
    PyObject *text = PyUnicode_DecodeUTF8(cursor, close - cursor, "strict");
    if (text != NULL && PyUnicode_GET_LENGTH(text) > -type) {
        Py_DECREF(text);
        return NULL;
    }
    *position = close + 1;
    return text;
}

static PyObject *parse_insert_rows(PyObject *self, PyObject *args)
{
    PyObject *sql, *schema;
    Py_ssize_t offset, size;
    if (!PyArg_ParseTuple(args, "UnO!", &sql, &offset, &PyTuple_Type, &schema))
        return NULL;
    const char *text = PyUnicode_AsUTF8AndSize(sql, &size);
    if (text == NULL) {
        if (PyErr_ExceptionMatches(PyExc_UnicodeEncodeError)) {
            PyErr_Clear();
            Py_RETURN_NONE;
        }
        return NULL;
    }
    if (offset < 0 || offset >= size || memchr(text, '\0', size) != NULL)
        Py_RETURN_NONE;
    Py_ssize_t columns = PyTuple_GET_SIZE(schema);
    if (columns <= 0)
        Py_RETURN_NONE;
    Py_ssize_t *types = PyMem_New(Py_ssize_t, columns);
    if (types == NULL)
        return PyErr_NoMemory();
    for (Py_ssize_t i = 0; i < columns; ++i) {
        types[i] = PyLong_AsSsize_t(PyTuple_GET_ITEM(schema, i));
        if (PyErr_Occurred()) {
            PyMem_Free(types);
            return NULL;
        }
    }
    PyObject *rows = PyList_New(0);
    PyObject *row = NULL;
    if (rows == NULL)
        goto fallback;
    const char *cursor = text + offset;
    const char *end = text + size;
    for (;;) {
        cursor = skip_sql_space(cursor);
        if (*cursor++ != '(')
            goto fallback;
        row = PyList_New(columns);
        if (row == NULL)
            goto fallback;
        for (Py_ssize_t i = 0; i < columns; ++i) {
            PyObject *value = read_sql_literal(&cursor, end, types[i]);
            if (value == NULL)
                goto fallback;
            PyList_SET_ITEM(row, i, value);
            cursor = skip_sql_space(cursor);
            if (*cursor++ != (i + 1 == columns ? ')' : ','))
                goto fallback;
        }
        if (PyList_Append(rows, row) < 0)
            goto fallback;
        Py_CLEAR(row);
        cursor = skip_sql_space(cursor);
        if (*cursor != ',')
            break;
        ++cursor;
    }
    if (*cursor == ';')
        cursor = skip_sql_space(cursor + 1);
    if (*cursor != '\0')
        goto fallback;
    PyMem_Free(types);
    return rows;

fallback:
    PyMem_Free(types);
    Py_XDECREF(row);
    Py_XDECREF(rows);
    if (PyErr_Occurred())
        return NULL;
    Py_RETURN_NONE;
}

static void free_scan(PyObject *capsule)
{
    Scan *scan = PyCapsule_GetPointer(capsule, "mvdb.scan");
    Py_DECREF(scan->vectors);
    PyMem_Free(scan);
}

static PyObject *build_scan(PyObject *self, PyObject *args)
{
    PyObject *vectors;
    Py_ssize_t dimension;
    if (!PyArg_ParseTuple(args, "O!n", &PyTuple_Type, &vectors, &dimension))
        return NULL;
    Py_ssize_t count = PyTuple_GET_SIZE(vectors);
    if (dimension <= 0 || dimension > PY_SSIZE_T_MAX / (Py_ssize_t)sizeof(float) ||
        count > (PY_SSIZE_T_MAX - (Py_ssize_t)sizeof(Scan)) / (Py_ssize_t)sizeof(float *)) {
        PyErr_SetString(PyExc_ValueError, "invalid vector scan dimensions");
        return NULL;
    }
    Scan *scan = PyMem_Malloc(sizeof(Scan) + count * sizeof(float *));
    if (scan == NULL)
        return PyErr_NoMemory();
    scan->count = count;
    scan->dimension = dimension;
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject *vector = PyTuple_GET_ITEM(vectors, i);
        if (!PyBytes_Check(vector) ||
            PyBytes_GET_SIZE(vector) != dimension * (Py_ssize_t)sizeof(float)) {
            PyMem_Free(scan);
            PyErr_SetString(PyExc_ValueError, "invalid vector buffer");
            return NULL;
        }
        scan->data[i] = (const float *)PyBytes_AS_STRING(vector);
    }
    PyObject *capsule = PyCapsule_New(scan, "mvdb.scan", free_scan);
    if (capsule == NULL) {
        PyMem_Free(scan);
        return NULL;
    }
    scan->vectors = Py_NewRef(vectors);
    return capsule;
}

static double squared_l2(const float *row, const float *query, Py_ssize_t dimension)
{
    double sum = 0.0;
    for (Py_ssize_t i = 0; i < dimension; ++i) {
        double delta = (double)row[i] - (double)query[i];
        sum += delta * delta;
    }
    return sum;
}

#ifdef HAVE_AVX2
__attribute__((target("avx2")))
static double squared_l2_avx2(const float *row, const float *query, Py_ssize_t dimension)
{
    __m256d a = _mm256_setzero_pd();
    __m256d b = _mm256_setzero_pd();
    __m256d c = _mm256_setzero_pd();
    __m256d d = _mm256_setzero_pd();
    Py_ssize_t i = 0;
    for (; i + 16 <= dimension; i += 16) {
        __m256d x = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(row + i)),
                                  _mm256_cvtps_pd(_mm_loadu_ps(query + i)));
        __m256d y = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(row + i + 4)),
                                  _mm256_cvtps_pd(_mm_loadu_ps(query + i + 4)));
        __m256d z = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(row + i + 8)),
                                  _mm256_cvtps_pd(_mm_loadu_ps(query + i + 8)));
        __m256d w = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(row + i + 12)),
                                  _mm256_cvtps_pd(_mm_loadu_ps(query + i + 12)));
        a = _mm256_add_pd(a, _mm256_mul_pd(x, x));
        b = _mm256_add_pd(b, _mm256_mul_pd(y, y));
        c = _mm256_add_pd(c, _mm256_mul_pd(z, z));
        d = _mm256_add_pd(d, _mm256_mul_pd(w, w));
    }
    double lanes[4];
    _mm256_storeu_pd(lanes, _mm256_add_pd(_mm256_add_pd(a, b), _mm256_add_pd(c, d)));
    return ((lanes[0] + lanes[1]) + (lanes[2] + lanes[3])) +
           squared_l2(row + i, query + i, dimension - i);
}

typedef struct {
    __m256 a, b, c, d;
} DistanceLanes;

__attribute__((target("avx2,fma")))
static inline DistanceLanes accumulate_l2(const float *row, const float *query,
                                           Py_ssize_t first, Py_ssize_t stop, DistanceLanes acc)
{
    for (Py_ssize_t i = first; i < stop; i += 32) {
        __m256 x = _mm256_sub_ps(_mm256_loadu_ps(row + i), _mm256_loadu_ps(query + i));
        __m256 y = _mm256_sub_ps(_mm256_loadu_ps(row + i + 8), _mm256_loadu_ps(query + i + 8));
        __m256 z = _mm256_sub_ps(_mm256_loadu_ps(row + i + 16), _mm256_loadu_ps(query + i + 16));
        __m256 w = _mm256_sub_ps(_mm256_loadu_ps(row + i + 24), _mm256_loadu_ps(query + i + 24));
        acc.a = _mm256_fmadd_ps(x, x, acc.a);
        acc.b = _mm256_fmadd_ps(y, y, acc.b);
        acc.c = _mm256_fmadd_ps(z, z, acc.c);
        acc.d = _mm256_fmadd_ps(w, w, acc.d);
    }
    return acc;
}

__attribute__((target("avx2,fma")))
static inline float sum_l2(DistanceLanes acc)
{
    __m256 total = _mm256_add_ps(_mm256_add_ps(acc.a, acc.b), _mm256_add_ps(acc.c, acc.d));
    __m128 lanes = _mm_add_ps(_mm256_castps256_ps128(total), _mm256_extractf128_ps(total, 1));
    lanes = _mm_add_ps(lanes, _mm_movehl_ps(lanes, lanes));
    return _mm_cvtss_f32(_mm_add_ss(lanes, _mm_shuffle_ps(lanes, lanes, 1)));
}

__attribute__((target("avx2,fma")))
static inline float estimate_squared_l2(const float *row, const float *query, Py_ssize_t dimension)
{
    __m256 zero = _mm256_setzero_ps();
    DistanceLanes acc = {zero, zero, zero, zero};
    return sum_l2(accumulate_l2(row, query, 0, dimension, acc));
}

__attribute__((target("avx2,fma")))
static inline float estimate_128(const float *row, const float *query, Py_ssize_t dimension)
{
    return estimate_squared_l2(row, query, 128);
}
#endif

static double (*compute_squared_l2)(const float *, const float *, Py_ssize_t) = squared_l2;
typedef float (*EstimateL2)(const float *, const float *, Py_ssize_t);
static int use_fma;

static double precise_l2(const float *row, const float *query, Py_ssize_t dimension)
{
    long double sum = 0.0L;
    long double error = 0.0L;
    for (Py_ssize_t i = 0; i < dimension; ++i) {
        long double delta = (long double)row[i] - (long double)query[i];
        long double square = delta * delta;
        long double next = sum + square;
        error += sum >= square ? (sum - next) + square : (square - next) + sum;
        sum = next;
    }
    return (double)sqrtl(sum + error);
}

static int better(Hit a, Hit b, int descending)
{
    if (a.distance == b.distance)
        return a.slot < b.slot;
    return descending ? a.distance > b.distance : a.distance < b.distance;
}

static void sift_down(Hit *heap, Py_ssize_t length, Py_ssize_t index, int descending)
{
    Hit hit = heap[index];
    while (index < length / 2) {
        Py_ssize_t child = 2 * index + 1;
        if (child + 1 < length && better(heap[child], heap[child + 1], descending))
            ++child;
        if (!better(hit, heap[child], descending))
            break;
        heap[index] = heap[child];
        index = child;
    }
    heap[index] = hit;
}

static int ambiguous(Hit a, Hit b, const Scan *scan, double tolerance)
{
    double largest = a.distance > b.distance ? a.distance : b.distance;
    return fabs(a.distance - b.distance) <= largest * tolerance &&
           memcmp(scan->data[a.slot], scan->data[b.slot], scan->dimension * sizeof(float)) != 0;
}

static float screening_threshold(double distance, Py_ssize_t dimension, int descending)
{
    // Each component contributes a subtraction and a square. Each positive
    // sum has at most dimension + 5 roundings. Allow a wider envelope for
    // those relative errors, subnormal products, and the float64 heap keys.
    double relative = (dimension + 32.0) * (4.0 * FLT_EPSILON);
    double absolute = (dimension + 32.0) * FLT_MIN;
    double squared = distance * distance;
    return (float)(descending ? squared * (1.0 - relative) - absolute
                              : squared * (1.0 + relative) + absolute);
}

__attribute__((always_inline))
static inline int collect_hits_core(Hit *heap, Py_ssize_t limit, const Scan *scan, const float *query,
                                    const uint32_t *slots, Py_ssize_t begin, Py_ssize_t count,
                                    int descending, int precise, EstimateL2 estimate)
{
    Py_ssize_t dimension = scan->dimension;
    double tolerance = (dimension + 8.0) * (4.0 * DBL_EPSILON);
    int screen = estimate != NULL && dimension >= 64 && dimension <= 4096 &&
                 dimension % 32 == 0 && limit < count;
    float threshold = 0.0f;
    for (Py_ssize_t i = 0; i < count; ++i) {
        Py_ssize_t slot = slots == NULL ? begin + i : slots[begin + i];
        if (slot >= scan->count)
            return -1;
        const float *vector = scan->data[slot];
        if (screen && i >= limit) {
            float squared = estimate(vector, query, dimension);
            if (isfinite(squared) && (descending ? squared < threshold : squared > threshold))
                continue;
        }
        double distance = precise ? precise_l2(vector, query, dimension)
                                  : sqrt(compute_squared_l2(vector, query, dimension));
        Hit hit = {slot, distance};
        if (i < limit) {
            heap[i] = hit;
            if (i + 1 == limit) {
                for (Py_ssize_t j = limit / 2; j > 0; --j)
                    sift_down(heap, limit, j - 1, descending);
                if (screen)
                    threshold = screening_threshold(heap[0].distance, dimension, descending);
            }
            continue;
        }
        if (!precise && ambiguous(hit, heap[0], scan, tolerance))
            return 1;
        if (better(hit, heap[0], descending)) {
            heap[0] = hit;
            sift_down(heap, limit, 0, descending);
            if (screen)
                threshold = screening_threshold(heap[0].distance, dimension, descending);
        }
    }
    for (Py_ssize_t i = limit - 1; i > 0; --i) {
        Hit last = heap[0];
        heap[0] = heap[i];
        sift_down(heap, i, 0, descending);
        heap[i] = last;
        if (!precise && ambiguous(heap[0], last, scan, tolerance))
            return 1;
    }
    return 0;
}

#ifdef HAVE_AVX2
__attribute__((target("avx2,fma")))
static int collect_fma_hits(Hit *heap, Py_ssize_t limit, const Scan *scan, const float *query,
                            const uint32_t *slots, Py_ssize_t begin, Py_ssize_t count,
                            int descending, int precise)
{
    if (scan->dimension == 128)
        return collect_hits_core(heap, limit, scan, query, slots, begin, count, descending,
                                 precise, estimate_128);
    return collect_hits_core(heap, limit, scan, query, slots, begin, count, descending,
                             precise, estimate_squared_l2);
}
#endif

static int collect_hits(Hit *heap, Py_ssize_t limit, const Scan *scan, const float *query,
                        const uint32_t *slots, Py_ssize_t begin, Py_ssize_t count,
                        int descending, int precise)
{
#ifdef HAVE_AVX2
    if (use_fma)
        return collect_fma_hits(heap, limit, scan, query, slots, begin, count, descending, precise);
#endif
    return collect_hits_core(heap, limit, scan, query, slots, begin, count, descending, precise, NULL);
}

#ifdef __linux__
typedef struct {
    const Scan *scan;
    const float *query;
    const uint32_t *slots;
    Hit *hits;
    Py_ssize_t first, count, limit;
    int descending;
    int status;
} ScanJob;

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_cond_t finished;
    pthread_t thread;
    ScanJob *job;
    int started, stopping;
} worker = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER};

static int parallel_allowed;

static int cpu_budget_allows_parallel(void)
{
    const char *threads = getenv("MVDB_SCAN_THREADS");
    cpu_set_t cpus;
    if ((threads != NULL && strcmp(threads, "1") == 0) ||
        sched_getaffinity(0, sizeof(cpus), &cpus) != 0 || CPU_COUNT(&cpus) < 2)
        return 0;
    FILE *handle = fopen("/sys/fs/cgroup/cpu.max", "r");
    if (handle == NULL)
        return 1;
    char quota[32];
    long long period;
    int fields = fscanf(handle, "%31s %lld", quota, &period);
    fclose(handle);
    return fields != 2 || strcmp(quota, "max") == 0 || strtoll(quota, NULL, 10) > period;
}

// Only the protocol thread accesses Python objects. Each job borrows immutable
// vector bytes while the caller holds the GIL and waits for both scans to finish.
static void *scan_worker(void *unused)
{
    pthread_mutex_lock(&worker.mutex);
    while (!worker.stopping) {
        if (worker.job == NULL) {
            pthread_cond_wait(&worker.ready, &worker.mutex);
            continue;
        }
        ScanJob *job = worker.job;
        pthread_mutex_unlock(&worker.mutex);
        job->status = collect_hits(job->hits, job->limit, job->scan, job->query,
                                   job->slots, job->first, job->count, job->descending, 0);
        pthread_mutex_lock(&worker.mutex);
        worker.job = NULL;
        pthread_cond_signal(&worker.finished);
    }
    pthread_mutex_unlock(&worker.mutex);
    return NULL;
}

static void stop_worker(void *module)
{
    if (!worker.started)
        return;
    pthread_mutex_lock(&worker.mutex);
    worker.stopping = 1;
    pthread_cond_signal(&worker.ready);
    pthread_mutex_unlock(&worker.mutex);
    pthread_join(worker.thread, NULL);
    worker.started = 0;
    worker.stopping = 0;
}

static int collect_parallel(Hit *result, Py_ssize_t limit, const Scan *scan,
                            const float *query, const uint32_t *slots,
                            Py_ssize_t count, int descending)
{
    if (!worker.started) {
        if (pthread_create(&worker.thread, NULL, scan_worker, NULL) != 0) {
            parallel_allowed = 0;
            return collect_hits(result, limit, scan, query, slots, 0, count, descending, 0);
        }
        worker.started = 1;
    }
    Hit *left = result + limit;
    Hit *right = left + limit;
    Py_ssize_t middle = count / 2;
    ScanJob job = {scan, query, slots, right, middle, count - middle, limit, descending, 0};
    pthread_mutex_lock(&worker.mutex);
    worker.job = &job;
    pthread_cond_signal(&worker.ready);
    pthread_mutex_unlock(&worker.mutex);

    int status = collect_hits(left, limit, scan, query, slots, 0, middle, descending, 0);
    pthread_mutex_lock(&worker.mutex);
    while (worker.job != NULL)
        pthread_cond_wait(&worker.finished, &worker.mutex);
    pthread_mutex_unlock(&worker.mutex);
    if (status < 0 || job.status < 0)
        return -1;
    if (status != 0 || job.status != 0)
        return 1;

    double tolerance = (scan->dimension + 8.0) * (4.0 * DBL_EPSILON);
    Py_ssize_t i = 0, j = 0;
    for (Py_ssize_t out = 0; out < limit; ++out) {
        // Both halves have at least K results, so their heads exist here.
        if (ambiguous(left[i], right[j], scan, tolerance))
            return 1;
        result[out] = better(left[i], right[j], descending) ? left[i++] : right[j++];
    }
    return 0;
}
#endif

static PyObject *topk(PyObject *self, PyObject *args)
{
    PyObject *capsule, *query, *candidate_buffer;
    Py_ssize_t limit;
    int descending;
    if (!PyArg_ParseTuple(args, "OSOnp", &capsule, &query, &candidate_buffer, &limit, &descending))
        return NULL;
    Scan *scan = PyCapsule_GetPointer(capsule, "mvdb.scan");
    if (scan == NULL)
        return NULL;
    if (limit < 0 || PyBytes_GET_SIZE(query) != scan->dimension * (Py_ssize_t)sizeof(float)) {
        PyErr_SetString(PyExc_ValueError, "invalid TopK arguments");
        return NULL;
    }
    Py_ssize_t count = scan->count;
    Py_buffer buffer = {0};
    if (candidate_buffer != Py_None) {
        if (PyObject_GetBuffer(candidate_buffer, &buffer, PyBUF_SIMPLE) < 0)
            return NULL;
        count = buffer.len / sizeof(uint32_t);
    }
    const uint32_t *slots = buffer.buf;
    if (limit > count)
        limit = count;
    PyObject *result = NULL;
    Hit *heap = NULL;
    if (limit == 0) {
        result = PyList_New(0);
        goto done;
    }
    int parallel = 0;
#ifdef __linux__
    parallel = parallel_allowed && limit <= 128 && count >= 4096 && scan->dimension >= 64;
#endif
    size_t workspace = sizeof(Hit) * (parallel ? 3 : 1);
    if (limit > PY_SSIZE_T_MAX / (Py_ssize_t)workspace ||
        (heap = PyMem_Malloc(limit * workspace)) == NULL) {
        PyErr_NoMemory();
        goto done;
    }
    const float *target = (const float *)PyBytes_AS_STRING(query);
    int status;
#ifdef __linux__
    if (parallel)
        status = collect_parallel(heap, limit, scan, target, slots, count, descending);
    else
#endif
        status = collect_hits(heap, limit, scan, target, slots, 0, count, descending, 0);
    // SIMD may round differently when equally distant vectors have permuted dimensions.
    if (status > 0)
        status = collect_hits(heap, limit, scan, target, slots, 0, count, descending, 1);
    if (status < 0) {
        PyErr_SetString(PyExc_ValueError, "invalid vector candidate slot");
        goto done;
    }
    result = PyList_New(limit);
    if (result == NULL)
        goto done;
    for (Py_ssize_t i = 0; i < limit; ++i) {
        PyObject *pair = Py_BuildValue("(nd)", heap[i].slot, heap[i].distance);
        if (pair == NULL) {
            Py_CLEAR(result);
            goto done;
        }
        PyList_SET_ITEM(result, i, pair);
    }

done:
    PyMem_Free(heap);
    if (buffer.obj != NULL)
        PyBuffer_Release(&buffer);
    return result;
}

static PyMethodDef methods[] = {
    {"parse_vector", parse_vector, METH_VARARGS, NULL},
    {"parse_insert_rows", parse_insert_rows, METH_VARARGS, NULL},
    {"build_scan", build_scan, METH_VARARGS, NULL},
    {"topk", topk, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "_mvdb_native", NULL, -1, methods,
#ifdef __linux__
    .m_free = stop_worker,
#endif
};

PyMODINIT_FUNC PyInit__mvdb_native(void)
{
#ifdef HAVE_AVX2
    if (__builtin_cpu_supports("avx2"))
        compute_squared_l2 = squared_l2_avx2;
    use_fma = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
#ifdef __linux__
    parallel_allowed = cpu_budget_allows_parallel();
#endif
    return PyModule_Create(&module);
}
