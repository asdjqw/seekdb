#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#define HAVE_AVX2 1
#endif

typedef struct {
    PyObject *vectors;
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
        1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15
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
    if (count == 0 || decimal_digit(*cursor) || *cursor == 'e' || *cursor == 'E' ||
        (digits[0] == '0' && (*cursor == 'x' || *cursor == 'X')))
        return strtod(start, end);
    *end = (char *)cursor;
    double magnitude = (double)coefficient / powers_of_ten[fractional];
    return *start == '-' ? -magnitude : magnitude;
}

static PyObject *parse_vector(PyObject *self, PyObject *args)
{
    const char *cursor;
    Py_ssize_t dimension;
    if (!PyArg_ParseTuple(args, "sn", &cursor, &dimension))
        return NULL;
    if (dimension <= 0 || dimension > PY_SSIZE_T_MAX / (Py_ssize_t)sizeof(float))
        Py_RETURN_NONE;
    while (isspace((unsigned char)*cursor))
        ++cursor;
    if (*cursor++ != '[')
        Py_RETURN_NONE;

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
    if (*cursor == '\0')
        return result;

fallback:
    Py_DECREF(result);
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
#endif

static double (*compute_squared_l2)(const float *, const float *, Py_ssize_t) = squared_l2;

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

static int collect_hits(Hit *heap, Py_ssize_t limit, const Scan *scan, const float *query,
                        const uint32_t *slots, Py_ssize_t count, int descending, int precise)
{
    Py_ssize_t size = PyTuple_GET_SIZE(scan->vectors);
    double tolerance = (scan->dimension + 8.0) * (4.0 * DBL_EPSILON);
    for (Py_ssize_t i = 0; i < count; ++i) {
        Py_ssize_t slot = slots == NULL ? i : slots[i];
        if (slot >= size) {
            PyErr_SetString(PyExc_ValueError, "invalid vector candidate slot");
            return -1;
        }
        const float *vector = scan->data[slot];
        double distance = precise ? precise_l2(vector, query, scan->dimension)
                                  : sqrt(compute_squared_l2(vector, query, scan->dimension));
        Hit hit = {slot, distance};
        if (i < limit) {
            heap[i] = hit;
            if (i + 1 == limit) {
                for (Py_ssize_t j = limit / 2; j > 0; --j)
                    sift_down(heap, limit, j - 1, descending);
            }
            continue;
        }
        if (!precise && ambiguous(hit, heap[0], scan, tolerance))
            return 1;
        if (better(hit, heap[0], descending)) {
            heap[0] = hit;
            sift_down(heap, limit, 0, descending);
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
    Py_ssize_t size = PyTuple_GET_SIZE(scan->vectors);
    Py_ssize_t count = size;
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
    if (limit > PY_SSIZE_T_MAX / (Py_ssize_t)sizeof(Hit) ||
        (heap = PyMem_Malloc(limit * sizeof(Hit))) == NULL) {
        PyErr_NoMemory();
        goto done;
    }
    const float *target = (const float *)PyBytes_AS_STRING(query);
    int status = collect_hits(heap, limit, scan, target, slots, count, descending, 0);
    // SIMD may round differently when equally distant vectors have permuted dimensions.
    if (status > 0)
        status = collect_hits(heap, limit, scan, target, slots, count, descending, 1);
    if (status < 0)
        goto done;
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
    {"build_scan", build_scan, METH_VARARGS, NULL},
    {"topk", topk, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "_mvdb_native", NULL, -1, methods
};

PyMODINIT_FUNC PyInit__mvdb_native(void)
{
#ifdef HAVE_AVX2
    if (__builtin_cpu_supports("avx2"))
        compute_squared_l2 = squared_l2_avx2;
#endif
    return PyModule_Create(&module);
}
