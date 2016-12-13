#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <float.h>
#include <math.h>
#include "kautodiff.h"

kad_drand_f kad_drand = drand48;

/**********************
 * Graph construction *
 **********************/

static inline kad_node_t *kad_new_core(int n_d, int op, int n_child)
{
	kad_node_t *s;
	if (n_d > KAD_MAX_DIM) return 0;
	s = (kad_node_t*)calloc(1, sizeof(kad_node_t));
	s->n_d = n_d, s->op = op, s->n_child = n_child;
	if (s->n_child) s->child = (kad_edge_t*)calloc(s->n_child, sizeof(kad_edge_t));
	return s;
}

kad_node_t *kad_par(float *x, int n_d, ...)
{
	int i;
	kad_node_t *p;
	va_list ap;
	if (n_d > KAD_MAX_DIM) return 0;
	va_start(ap, n_d);
	p = kad_new_core(n_d, 0, 0);
	for (i = 0; i < n_d; ++i)
		p->d[i] = va_arg(ap, int);
	p->x = x;
	va_end(ap);
	return p;
}

kad_node_t *kad_var(float *x, float *g, int n_d, ...)
{
	int i;
	kad_node_t *p;
	va_list ap;
	if (n_d > KAD_MAX_DIM) return 0;
	va_start(ap, n_d);
	p = kad_new_core(n_d, 0, 0);
	for (i = 0; i < n_d; ++i)
		p->d[i] = va_arg(ap, int);
	p->x = x, p->g = g, p->to_back = 1;
	va_end(ap);
	return p;
}

static inline kad_node_t *kad_op2_core(int op, kad_node_t *x, kad_node_t *y)
{
	kad_node_t *s;
	s = kad_new_core(0, op, 2);
	s->child[0].p = x, s->child[1].p = y;
	if (kad_op_list[op](s, KAD_SYNC_DIM) < 0) {
		free(s->child); free(s);
		return 0;
	}
	return s;
}

static inline kad_node_t *kad_op1_core(int op, kad_node_t *x)
{
	kad_node_t *s;
	s = kad_new_core(0, op, 1);
	s->child[0].p = x;
	kad_op_list[op](s, KAD_SYNC_DIM);
	return s;
}

#define KAD_FUNC_OP2(fname, op) kad_node_t *fname(kad_node_t *x, kad_node_t *y) { return kad_op2_core((op), x, y); }

KAD_FUNC_OP2(kad_add, 1)
KAD_FUNC_OP2(kad_mul, 2)
KAD_FUNC_OP2(kad_cmul, 3)
KAD_FUNC_OP2(kad_ceb, 4)
KAD_FUNC_OP2(kad_cem, 12)
KAD_FUNC_OP2(kad_softmax2, 13)
KAD_FUNC_OP2(kad_dropout, 15)

#define KAD_FUNC_OP1(fname, op) kad_node_t *fname(kad_node_t *x) { return kad_op1_core((op), x); }

KAD_FUNC_OP1(kad_norm2, 5)
KAD_FUNC_OP1(kad_sigm, 6)
KAD_FUNC_OP1(kad_tanh, 7)
KAD_FUNC_OP1(kad_relu, 8)
KAD_FUNC_OP1(kad_1minus, 11)
KAD_FUNC_OP1(kad_softmax, 14)

/////////// Pooling ///////////

static kad_node_t *kad_op_pooling_core(int op, int n, kad_node_t **x)
{
	int i;
	kad_node_t *s;
	s = kad_new_core(0, op, n);
	for (i = 0; i < n; ++i)
		s->child[i].p = x[i];
	if (kad_op_list[op](s, KAD_SYNC_DIM) < 0) {
		free(s->child); free(s);
		return 0;
	}
	return s;
}

kad_node_t *kad_avg(int n, kad_node_t **x) { return kad_op_pooling_core(10, n, x); }
kad_node_t *kad_max(int n, kad_node_t **x) { return kad_op_pooling_core(21, n, x); }

/////////// Convolution ///////////

typedef struct {
	int c_stride, r_stride;
	int c_pad, r_pad;
	int kernel_h, kernel_w;
} kad_conv2d_t;

kad_node_t *kad_conv2d(kad_node_t *x, kad_node_t *w, int r_stride, int c_stride, int r_pad, int c_pad)
{
	kad_node_t *s;
	kad_conv2d_t *cnn;
	assert(r_pad == 0 && c_pad == 0); // not implemented yet
	if (w->n_d != 4) return 0;
	s = kad_new_core(0, 16, 2);
	s->child[0].p = x, s->child[1].p = w;
	cnn = (kad_conv2d_t*)calloc(1, sizeof(kad_conv2d_t));
	cnn->kernel_h = w->d[0], cnn->r_stride = r_stride, cnn->r_pad = r_pad;
	cnn->kernel_w = w->d[1], cnn->c_stride = c_stride, cnn->c_pad = c_pad;
	s->ptr = cnn, s->ptr_size = sizeof(kad_conv2d_t);
	if (kad_op_list[16](s, KAD_SYNC_DIM) < 0) {
		free(cnn); free(s->child); free(s);
		return 0;
	}
	return s;
}

kad_node_t *kad_max2d(kad_node_t *x, int kernel_h, int kernel_w, int r_stride, int c_stride, int r_pad, int c_pad)
{
	kad_node_t *s;
	kad_conv2d_t *cnn;
	assert(r_pad == 0 && c_pad == 0); // not implemented yet
	s = kad_new_core(0, 17, 1);
	s->child[0].p = x;
	cnn = (kad_conv2d_t*)calloc(1, sizeof(kad_conv2d_t));
	cnn->kernel_h = kernel_h, cnn->r_stride = r_stride, cnn->r_pad = r_pad;
	cnn->kernel_w = kernel_w, cnn->c_stride = c_stride, cnn->c_pad = c_pad;
	s->ptr = cnn, s->ptr_size = sizeof(kad_conv2d_t);
	if (kad_op_list[17](s, KAD_SYNC_DIM) < 0) {
		free(cnn); free(s->child); free(s);
		return 0;
	}
	return s;
}

typedef struct {
	int stride, pad;
	int kernel_size; // for max
} kad_conv1d_t;

kad_node_t *kad_conv1d(kad_node_t *x, kad_node_t *w, int stride, int pad)
{
	kad_node_t *s;
	kad_conv1d_t *cnn;
	assert(pad == 0); // not implemented yet
	s = kad_new_core(0, 18, 2);
	s->child[0].p = x, s->child[1].p = w;
	cnn = (kad_conv1d_t*)calloc(1, sizeof(kad_conv1d_t));
	cnn->stride = stride, cnn->pad = pad, cnn->kernel_size = w->d[0];
	s->ptr = cnn, s->ptr_size = sizeof(kad_conv1d_t);
	if (kad_op_list[18](s, KAD_SYNC_DIM) < 0) {
		free(cnn); free(s->child); free(s);
		return 0;
	}
	return s;
}

kad_node_t *kad_max1d(kad_node_t *x, int kernel_size, int stride, int pad)
{
	kad_node_t *s;
	kad_conv1d_t *cnn;
	assert(pad == 0); // not implemented yet
	s = kad_new_core(0, 19, 1);
	s->child[0].p = x;
	cnn = (kad_conv1d_t*)calloc(1, sizeof(kad_conv1d_t));
	cnn->stride = stride, cnn->pad = pad, cnn->kernel_size = kernel_size;
	s->ptr = cnn, s->ptr_size = sizeof(kad_conv1d_t);
	if (kad_op_list[19](s, KAD_SYNC_DIM) < 0) {
		free(cnn); free(s->child); free(s);
		return 0;
	}
	return s;
}

/////////// Miscellaneous ///////////

kad_node_t *kad_split(kad_node_t *x, int dim, int start, int end)
{
	kad_node_t *s;
	int32_t *aux;
	if (end < start || start < 0) return 0;
	aux = (int32_t*)malloc(3 * 4);
	aux[0] = dim, aux[1] = start, aux[2] = end;
	s = kad_new_core(0, 20, 1);
	s->child[0].p = x;
	s->ptr = aux, s->ptr_size = 3 * 4;
	if (kad_op_list[20](s, KAD_SYNC_DIM) < 0) {
		free(aux); free(s->child); free(s);
		return 0;
	}
	return s;
}

/***********************
 * Graph linearization *
 ***********************/

static void kad_mark_back(int n, kad_node_t **v)
{
	int i, j;
	for (i = 0; i < n; ++i)
		for (j = 0; j < v[i]->n_child; ++j)
			if (v[i]->child[j].p->to_back)
				v[i]->to_back = 1;
}

void kad_allocate_internal(int n, kad_node_t **v)
{
	int i;
	kad_mark_back(n, v);
	for (i = 0; i < n; ++i) {
		kad_node_t *p = v[i];
		if (p->n_child == 0) continue;
		p->x = (float*)realloc(p->x, kad_len(p) * sizeof(float));
		if (p->to_back) {
			p->g = (float*)realloc(p->g, kad_len(p) * sizeof(float));
			kad_op_list[p->op](p, KAD_ALLOC);
		}
	}
}

#define kvec_t(type) struct { size_t n, m; type *a; }

#define kv_pop(v) ((v).a[--(v).n])

#define kv_push(type, v, x) do { \
		if ((v).n == (v).m) { \
			(v).m = (v).m? (v).m<<1 : 2; \
			(v).a = (type*)realloc((v).a, sizeof(type) * (v).m); \
		} \
		(v).a[(v).n++] = (x); \
	} while (0)

typedef struct kad_node_t *kad_node_p;

// IMPORTANT: kad_node_t::tmp MUST BE set to zero before calling this function
kad_node_t **kad_compile_array(int *n_node, int n_roots, kad_node_t **roots)
{
	int i;
	kvec_t(kad_node_p) stack = {0,0,0}, a = {0,0,0};

	// generate kad_node_t::tmp
	for (i = 0; i < n_roots; ++i) kv_push(kad_node_p, stack, roots[i]);
	while (stack.n) {
		kad_node_t *p = kv_pop(stack);
		for (i = 0; i < p->n_child; ++i) {
			kad_node_t *q = p->child[i].p;
			if (q->tmp == 0) kv_push(kad_node_p, stack, q);
			++q->tmp;
		}
	}
	for (i = 0; i < n_roots; ++i) // check if roots are really roots
		assert(roots[i]->tmp == 0);

	// topological sorting (Kahn's algorithm)
	for (i = 0; i < n_roots; ++i) kv_push(kad_node_p, stack, roots[i]);
	while (stack.n) {
		kad_node_t *p = kv_pop(stack);
		kv_push(kad_node_p, a, p);
		for (i = 0; i < p->n_child; ++i)
			if (--p->child[i].p->tmp == 0)
				kv_push(kad_node_p, stack, p->child[i].p);
	}
	free(stack.a);
	for (i = 0; i < a.n; ++i) // check cycles; no cycles if constructed with kad_add() etc
		assert(a.a[i]->tmp == 0);

	// post-processing: reverse, mark to_back and allocate memory for internal nodes
	for (i = 0; i < a.n>>1; ++i) { // reverse a.a[]
		kad_node_p t;
		t = a.a[i], a.a[i] = a.a[a.n-1-i], a.a[a.n-1-i] = t;
	}
	kad_allocate_internal(a.n, a.a);

	*n_node = a.n;
	return a.a;
}

kad_node_t **kad_compile(int *n_node, int n_roots, ...)
{
	int i;
	kad_node_t **roots;
	va_list ap;

	roots = (kad_node_t**)alloca(n_roots * sizeof(kad_node_t*));
	va_start(ap, n_roots);
	for (i = 0; i < n_roots; ++i) roots[i] = va_arg(ap, kad_node_p);
	va_end(ap);
	return kad_compile_array(n_node, n_roots, roots);
}

void kad_delete(int n, kad_node_t **a)
{
	int i, j;
	for (i = 0; i < n; ++i) {
		kad_node_t *p = a[i];
		for (j = 0; j < p->n_child; ++j)
			free(p->child[j].t);
		if (p->n_child) {
			free(p->x);
			free(p->g);
		}
		free(p->child);
		free(p->ptr);
		free(p);
	}
	free(a);
}

/**********************************
 * Computate values and gradients *
 **********************************/

static void kad_mark_compute(int n, kad_node_t **a)
{
	int i, j;
	for (i = n - 1; i >= 0; --i)
		if (a[i]->tmp)
			for (j = 0; j < a[i]->n_child; ++j)
				a[i]->child[j].p->tmp = 1;
}

static void kad_eval_core(int n, kad_node_t **a)
{
	int i;
	kad_mark_compute(n, a);
	for (i = 0; i < n; ++i)
		if (a[i]->n_child && a[i]->tmp)
			kad_op_list[a[i]->op](a[i], KAD_FORWARD);
	for (i = 0; i < n; ++i) a[i]->tmp = 0;
}

const float *kad_eval_from(int n, kad_node_t **a, int from)
{
	int i;
	if (from < 0 || from >= n) from = n - 1;
	for (i = 0; i < n; ++i) a[i]->tmp = (i == from);
	kad_eval_core(n, a);
	return a[from]->x;
}

void kad_eval_by_label(int n, kad_node_t **a, int label)
{
	int i;
	for (i = 0; i < n; ++i) a[i]->tmp = (a[i]->label == label);
	kad_eval_core(n, a);
}

void kad_grad(int n, kad_node_t **a, int from)
{
	int i;
	if (from < 0 || from >= n) from = n - 1;
	assert(a[from]->n_d == 0);
	for (i = 0; i < n; ++i) a[i]->tmp = (i == from);
	kad_mark_compute(n, a);
	for (i = 0; i <= from; ++i) // set all grandients to zero
		if (a[i]->g && a[i]->tmp) memset(a[i]->g, 0, kad_len(a[i]) * sizeof(float));
	for (i = from, a[i]->g[0] = 1.0f; i >= 0; --i) // backprop
		if (a[i]->n_child && a[i]->tmp)
			kad_op_list[a[i]->op](a[i], KAD_BACKWARD);
	for (i = 0; i <= from; ++i) a[i]->tmp = 0;
}

/***********************
 * Load and save graph *
 ***********************/

static void kad_write1(FILE *fp, const kad_node_t *p)
{
	fwrite(&p->label, 4, 1, fp);
	fwrite(&p->n_child, 4, 1, fp);
	if (p->n_child) {
		int32_t j, pre = p->pre? p->pre->tmp : -1;
		fwrite(&p->op, 2, 1, fp);
		for (j = 0; j < p->n_child; ++j)
			fwrite(&p->child[j].p->tmp, 4, 1, fp);
		fwrite(&pre, 4, 1, fp);
		fwrite(&p->ptr_size, 4, 1, fp);
		if (p->ptr_size > 0 && p->ptr)
			fwrite(p->ptr, p->ptr_size, 1, fp);
	} else {
		fwrite(&p->n_d, 1, 1, fp);
		if (p->n_d) fwrite(p->d, 4, p->n_d, fp);
		fwrite(&p->to_back, 1, 1, fp);
	}
}

static kad_node_t *kad_read1(FILE *fp, kad_node_t **node)
{
	kad_node_t *p;
	p = (kad_node_t*)calloc(1, sizeof(kad_node_t));
	fread(&p->label, 4, 1, fp);
	fread(&p->n_child, 4, 1, fp);
	if (p->n_child) {
		int32_t j, k;
		p->child = (kad_edge_t*)calloc(p->n_child, sizeof(kad_edge_t));
		fread(&p->op, 2, 1, fp);
		for (j = 0; j < p->n_child; ++j) {
			fread(&k, 4, 1, fp);
			p->child[j].p = node? node[k] : 0;
		}
		fread(&k, 4, 1, fp);
		if (k >= 0) p->pre = node[k];
		fread(&p->ptr_size, 4, 1, fp);
		if (p->ptr_size > 0) {
			p->ptr = malloc(p->ptr_size);
			fread(p->ptr, p->ptr_size, 1, fp);
		}
	} else {
		fread(&p->n_d, 1, 1, fp);
		if (p->n_d) fread(p->d, 4, p->n_d, fp);
		fread(&p->to_back, 1, 1, fp);
	}
	return p;
}

int kad_write(FILE *fp, int n_node, kad_node_t **node)
{
	int32_t i, k = n_node;
	fwrite(&k, 4, 1, fp);
	for (i = 0; i < n_node; ++i) node[i]->tmp = i;
	for (i = 0; i < n_node; ++i) kad_write1(fp, node[i]);
	for (i = 0; i < n_node; ++i) node[i]->tmp = 0;
	return 0;
}

kad_node_t **kad_read(FILE *fp, int *_n_node)
{
	int32_t i, n_node;
	kad_node_t **node;
	fread(&n_node, 4, 1, fp);
	node = (kad_node_t**)malloc(n_node * sizeof(kad_node_t*));
	for (i = 0; i < n_node; ++i) {
		kad_node_t *p;
		p = node[i] = kad_read1(fp, node);
		if (p->n_child) {
			kad_op_list[p->op](p, KAD_ALLOC);
			kad_op_list[p->op](p, KAD_SYNC_DIM);
		}
	}
	*_n_node = n_node;
	kad_mark_back(n_node, node);
	return node;
}

/*********************
 * Vector operations *
 *********************/

#ifdef __SSE__
#include <xmmintrin.h>

static inline float kad_sdot(int n, const float *x, const float *y) // BLAS sdot using SSE
{
	int i, n8 = n>>3<<3;
	__m128 vs1, vs2;
	float s, t[4];
	vs1 = _mm_setzero_ps();
	vs2 = _mm_setzero_ps();
	for (i = 0; i < n8; i += 8) {
		__m128 vx1, vx2, vy1, vy2;
		vx1 = _mm_loadu_ps(&x[i]);
		vx2 = _mm_loadu_ps(&x[i+4]);
		vy1 = _mm_loadu_ps(&y[i]);
		vy2 = _mm_loadu_ps(&y[i+4]);
		vs1 = _mm_add_ps(vs1, _mm_mul_ps(vx1, vy1));
		vs2 = _mm_add_ps(vs2, _mm_mul_ps(vx2, vy2));
	}
	for (s = 0.; i < n; ++i) s += x[i] * y[i];
	_mm_storeu_ps(t, vs1);
	s += t[0] + t[1] + t[2] + t[3];
	_mm_storeu_ps(t, vs2);
	s += t[0] + t[1] + t[2] + t[3];
	return s;
}
static inline void kad_saxpy(int n, float a, const float *x, float *y) // BLAS saxpy using SSE
{
	int i, n8 = n>>3<<3;
	__m128 va;
	va = _mm_set1_ps(a);
	for (i = 0; i < n8; i += 8) {
		__m128 vx1, vx2, vy1, vy2, vt1, vt2;
		vx1 = _mm_loadu_ps(&x[i]);
		vx2 = _mm_loadu_ps(&x[i+4]);
		vy1 = _mm_loadu_ps(&y[i]);
		vy2 = _mm_loadu_ps(&y[i+4]);
		vt1 = _mm_add_ps(_mm_mul_ps(va, vx1), vy1);
		vt2 = _mm_add_ps(_mm_mul_ps(va, vx2), vy2);
		_mm_storeu_ps(&y[i], vt1);
		_mm_storeu_ps(&y[i+4], vt2);
	}
	for (; i < n; ++i) y[i] += a * x[i];
}
#else
static inline float kad_sdot(int n, const float *x, const float *y) // BLAS sdot
{
	int i;
	float s = 0.;
	for (i = 0; i < n; ++i) s += x[i] * y[i];
	return s;
}
static inline void kad_saxpy(int n, float a, const float *x, float *y) // BLAS saxpy
{
	int i;
	for (i = 0; i < n; ++i) y[i] += a * x[i];
}
#endif

void kad_vec_mul_sum(int n, float *a, const float *b, const float *c)
{
	int i;
	for (i = 0; i < n; ++i) a[i] += b[i] * c[i];
}

void kad_sgemm_simple(int trans_A, int trans_B, int M, int N, int K, const float *A, const float *B, float *C) // simplified BLAS sgemm
{
	static const int x = 16;
	int i, j, k;
	if (!trans_A && trans_B) {
		for (i = 0; i < M; i += x) {
			for (j = 0; j < N; j += x) {
				int ii, ie = M < i + x? M : i + x;
				int jj, je = N < j + x? N : j + x;
				for (ii = i; ii < ie; ++ii) { // loop tiling
					const float *aii = A + ii * K, *bjj;
					float *cii = C + ii * N;
					for (jj = j, bjj = B + j * K; jj < je; ++jj, bjj += K)
						cii[jj] += kad_sdot(K, aii, bjj);
				}
			}
		}
	} else if (!trans_A && !trans_B) {
		for (i = 0; i < M; ++i)
			for (k = 0; k < K; ++k)
				kad_saxpy(N, A[i*K+k], &B[k*N], &C[i*N]);
	} else if (trans_A && !trans_B) {
		for (k = 0; k < K; ++k)
			for (i = 0; i < M; ++i)
				kad_saxpy(N, A[k*M+i], &B[k*N], &C[i*N]);
	} else abort(); // not implemented for (trans_A && trans_B)
}

/*************
 * Operators *
 *************/

static inline void kad_sync_dim1(kad_node_t *dst, const kad_node_t *src) // set the dimension/shape of dst to src
{
	dst->n_d = src->n_d;
	if (src->n_d) memcpy(dst->d, src->d, src->n_d * sizeof(int));
}

/////////// Arithmetic operations ///////////

int kad_op_add(kad_node_t *p, int action)
{
	int i, n0, n1;
	kad_node_t *q[2];

	q[0] = p->child[0].p, n0 = kad_len(q[0]);
	q[1] = p->child[1].p, n1 = kad_len(q[1]);
	if (action == KAD_SYNC_DIM) {
		if (n0 % n1 != 0) return -1;
		kad_sync_dim1(p, q[0]);
	} else if (action == KAD_FORWARD) {
		assert(n0 >= n1);
		memcpy(p->x, q[0]->x, n0 * sizeof(float));
		for (i = 0; i < n0; i += n1)
			kad_saxpy(n1, 1.0f, q[1]->x, p->x + i);
	} else if (action == KAD_BACKWARD) {
		if (q[0]->to_back) kad_saxpy(n0, 1.0f, p->g, q[0]->g);
		if (q[1]->to_back)
			for (i = 0; i < n0; i += n1)
				kad_saxpy(n1, 1.0f, p->g + i, q[1]->g);
	}
	return 0;
}

int kad_op_mul(kad_node_t *p, int action)
{
	int i, n0, n1;
	kad_node_t *q[2];

	q[0] = p->child[0].p, n0 = kad_len(q[0]);
	q[1] = p->child[1].p, n1 = kad_len(q[1]);
	if (action == KAD_SYNC_DIM) {
		if (n0 % n1 != 0) return -1;
		kad_sync_dim1(p, q[0]);
	} else if (action == KAD_FORWARD) {
		assert(n0 >= n1);
		memset(p->x, 0, n0 * sizeof(float));
		if (q[0]->x != 0 && q[1]->x != 0)
			for (i = 0; i < n0; i += n1) // TODO: optimize when n1==1
				kad_vec_mul_sum(n1, p->x + i, q[0]->x + i, q[1]->x);
	} else if (action == KAD_BACKWARD) {
		if (q[0]->to_back && q[1]->x)
			for (i = 0; i < n0; i += n1)
				kad_vec_mul_sum(n1, q[0]->g + i, p->g + i, q[1]->x);
		if (q[1]->to_back && q[0]->x)
			for (i = 0; i < n0; i += n1)
				kad_vec_mul_sum(n1, q[1]->g, p->g + i, q[0]->x + i);
	}
	return 0;
}

int kad_op_cmul(kad_node_t *p, int action)
{
	int n_a_row, n_b_row, n_col, n_a_col, n_b_col;
	kad_node_t *q[2];

	q[0] = p->child[0].p;
	q[1] = p->child[1].p;
	n_a_col = q[0]->n_d == 1? q[0]->d[0] : kad_len(q[0]) / q[0]->d[0];
	n_b_col = q[1]->n_d == 1? q[1]->d[0] : kad_len(q[1]) / q[1]->d[0];
	n_a_row = kad_len(q[0]) / n_a_col, n_b_row = kad_len(q[1]) / n_b_col;
	n_col = n_a_col;
	if (action == KAD_SYNC_DIM) {
		if (n_a_col != n_b_col) return -1;
		p->n_d = 2, p->d[0] = n_a_row, p->d[1] = n_b_row;
	} else if (action == KAD_FORWARD) {
		memset(p->x, 0, n_a_row * n_b_row * sizeof(float));
		if (q[0]->x && q[1]->x)
			kad_sgemm_simple(0, 1, n_a_row, n_b_row, n_col, q[0]->x, q[1]->x, p->x); // Y = X * trans(W)
	} else if (action == KAD_BACKWARD) {
		if (q[0]->to_back && q[1]->x)
			kad_sgemm_simple(0, 0, n_a_row, n_col, n_b_row, p->g, q[1]->x, q[0]->g); // G_x = G_y * W
		if (q[1]->to_back && q[0]->x)
			kad_sgemm_simple(1, 0, n_b_row, n_col, n_a_row, p->g, q[0]->x, q[1]->g); // G_w = trans(G_y) * X
	}
	return 0;
}

int kad_op_norm2(kad_node_t *p, int action)
{
	int i, n;
	kad_node_t *q = p->child[0].p;
	n = kad_len(q);
	if (action == KAD_SYNC_DIM) {
		p->n_d = 0;
	} else if (action == KAD_FORWARD) {
		p->x[0] = kad_sdot(n, q->x, q->x);
	} else if (action == KAD_BACKWARD) {
		if (q->to_back) {
			float s = 1.0f / n;
			for (i = 0; i < n; ++i)
				q->g[i] += s * p->g[0] * (q->x[i] + q->x[i]);
		}
	}
	return 0;
}

int kad_op_1minus(kad_node_t *p, int action)
{
	int i, n;
	kad_node_t *q = p->child[0].p;
	n = kad_len(q);
	if (action == KAD_SYNC_DIM) {
		kad_sync_dim1(p, q);
	} else if (action == KAD_FORWARD) {
		for (i = 0; i < n; ++i) p->x[i] = 1.0f - q->x[i];
	} else if (action == KAD_BACKWARD) {
		if (q->to_back)
			kad_saxpy(n, -1.0f, p->g, q->g);
	}
	return 0;
}

/////////// Miscellaneous ///////////

int kad_op_dropout(kad_node_t *p, int action)
{
	int i, n;
	kad_node_t *q = p->child[0].p;
	assert(p->child[1].p->n_d == 0);
	n = kad_len(q);
	if (action == KAD_SYNC_DIM) {
		kad_sync_dim1(p, q);
	} else if (action == KAD_ALLOC) {
		if (p->child[0].p->to_back)
			p->child[0].t = (float*)realloc(p->child[0].t, n);
	} else if (action == KAD_FORWARD) {
		float r = *p->child[1].p->x, z = 1.0f / (1.0f - r);
		unsigned char *flag = (unsigned char*)p->child[0].t;
		for (i = 0; i < n; ++i) {
			int kept = (kad_drand() >= r);
			p->x[i] = kept? q->x[i] * z : 0.0f;
			if (flag) flag[i] = kept;
		}
	} else if (action == KAD_BACKWARD) {
		unsigned char *flag = (unsigned char*)p->child[0].t;
		if (flag)
			for (i = 0; i < n; ++i)
				if (flag[i]) q->g[i] += p->g[i];
	}
	return 0;
}

int kad_op_split(kad_node_t *p, int action)
{
	kad_node_t *q = p->child[0].p;
	int32_t *aux, n, *range;
	int i, dim, d0, d1;

	assert(p->ptr);
	aux = (int*)p->ptr, dim = aux[0], range = aux + 1;
	if (dim < 0 || dim >= q->n_d) return -1;
	n = kad_len(q);
	for (i = 0, d0 = 1; i < dim; ++i) d0 *= q->d[i];
	for (i = dim + 1, d1 = 1; i < q->n_d; ++i) d1 *= q->d[i];
	if (action == KAD_SYNC_DIM) {
		if (range[0] >= range[1] || range[0] < 0 || range[1] > q->d[dim]) return -1;
		kad_sync_dim1(p, q);
		p->d[dim] = range[1] - range[0];
	} else if (action == KAD_FORWARD) {
		for (i = 0; i < d0; ++i)
			memcpy(&p->x[i * p->d[dim] * d1], &q->x[(i * q->d[dim] + range[0]) * d1], (range[1] - range[0]) * d1 * sizeof(float));
	} else if (action == KAD_BACKWARD && q->to_back) {
		for (i = 0; i < d0; ++i)
			kad_saxpy((range[1] - range[0]) * d1, 1.0f, &p->g[i * p->d[dim] * d1], &q->g[(i * q->d[dim] + range[0]) * d1]);
	}
	return 0;
}

/////////// Binary and multi-class cross-entropy ///////////

int kad_op_ceb(kad_node_t *p, int action)
{
	static const float tiny = 1e-9f;
	kad_edge_t *e[2];
	int i, n0, n1;

	e[0] = &p->child[0], e[1] = &p->child[1];
	assert(e[1]->p->to_back == 0); // child[1] is the true; we don't backprop this
	n0 = kad_len(e[0]->p);
	n1 = kad_len(e[1]->p);
	if (action == KAD_SYNC_DIM) {
		if (n0 != n1) return -1;
		p->n_d = 0;
	} else if (action == KAD_ALLOC) {
		if (e[0]->p->to_back)
			e[0]->t = (float*)realloc(e[0]->t, n0 * sizeof(float));
	} else if (action == KAD_FORWARD) {
		const float *x, *y;
		double s;
		x = e[0]->p->x, y = e[1]->p->x;
		for (i = 0, s = 0.0; i < n0; ++i) {
			float t, y1 = 1.0f - y[i];
			t = 1.0f / (1.0f + expf(-x[i]));
			if (e[0]->p->to_back) e[0]->t[i] = (t - y[i]) / n0;
			s -= (y[i] == 0.0f? 0.0f : y[i] * logf(t / y[i] + tiny)) + (y1 == 0.0f? 0.0f : y1 * logf((1.0f - t) / y1 + tiny));
		}
		p->x[0] = s / n0;
	} else if (action == KAD_BACKWARD) {
		if (e[0]->p->to_back)
			kad_saxpy(n0, p->g[0], e[0]->t, e[0]->p->g);
	}
	return 0;
}

int kad_op_cem(kad_node_t *p, int action)
{
	kad_edge_t *e[2];
	int i, j, n0, n1;

	e[0] = &p->child[0], e[1] = &p->child[1];
	assert(e[1]->p->to_back == 0); // child[1] is the true; we don't backprop this
	assert(e[0]->p->n_d == 2);
	n0 = kad_len(e[0]->p);
	n1 = kad_len(e[1]->p);
	if (action == KAD_SYNC_DIM) {
		if (n0 != n1) return -1;
		p->n_d = 0;
	} else if (action == KAD_ALLOC) {
		e[0]->t = (float*)realloc(e[0]->t, n0 * sizeof(float));
	} else if (action == KAD_FORWARD) {
		double cost;
		int r = e[0]->p->d[0], c = e[0]->p->d[1];
		for (i = 0; i < n0; ++i) e[0]->t[i] = expf(e[0]->p->x[i]); // FIXME: numerical stability!
		for (j = 0, cost = 0.0; j < r; ++j) {
			const float *x, *y;
			float *p, lsx, sx = 0.0f, sy = 0.0f;
			x = e[0]->p->x + j * c;
			y = e[1]->p->x + j * c;
			p = e[0]->t + j * c;
			for (i = 0; i < c; ++i)
				sx += p[i], sy += y[i];
			assert(sx > 0.0 && sy > 0.0);
			lsx = logf(sx);
			sx = 1.0f / sx, sy = 1.0f / sy;
			for (i = 0; i < c; ++i) {
				float yi = y[i] * sy;
				if (yi != 0.0f) cost += yi * (logf(yi) - (x[i] - lsx));
				p[i] = (p[i] * sx - yi) / r;
			}
		}
		p->x[0] = cost / r;
	} else if (action == KAD_BACKWARD) {
		if (e[0]->p->to_back)
			kad_saxpy(n0, p->g[0], e[0]->t, e[0]->p->g);
	}
	return 0;
}

/////////// Activation functions ///////////

int kad_op_sigm(kad_node_t *p, int action)
{
	int i, n;
	kad_node_t *q = p->child[0].p;
	n = kad_len(q);
	if (action == KAD_SYNC_DIM) {
		kad_sync_dim1(p, q);
	} else if (action == KAD_FORWARD) {
		for (i = 0; i < n; ++i)
			p->x[i] = 1.0f / (1.0f + expf(-q->x[i]));
	} else if (action == KAD_BACKWARD) {
		if (q->to_back) {
			float s = 1.0f / n;
			for (i = 0; i < n; ++i)
				q->g[i] += s * p->g[i] * (p->x[i] * (1.0f - p->x[i]));
		}
	}
	return 0;
}

int kad_op_tanh(kad_node_t *p, int action)
{
	int i, n;
	kad_node_t *q = p->child[0].p;
	n = kad_len(q);
	if (action == KAD_SYNC_DIM) {
		kad_sync_dim1(p, q);
	} else if (action == KAD_FORWARD) {
		for (i = 0; i < n; ++i) {
			if (q->x[i] < -20.0f) p->x[i] = -1.0f;
			else {
				float y;
				y = expf(-2.0f * q->x[i]);
				p->x[i] = (1.0f - y) / (1.0f + y);
			}
		}
	} else if (action == KAD_BACKWARD) {
		if (q->to_back)
			for (i = 0; i < n; ++i)
				q->g[i] += p->g[i] * (1.0f - p->x[i] * p->x[i]);
	}
	return 0;
}

int kad_op_relu(kad_node_t *p, int action)
{
	int i, n;
	kad_node_t *q = p->child[0].p;
	n = kad_len(q);
	if (action == KAD_SYNC_DIM) {
		kad_sync_dim1(p, q);
	} else if (action == KAD_FORWARD) {
		for (i = 0; i < n; ++i)
			p->x[i] = q->x[i] > 0.0f? q->x[i] : 0.0f;
	} else if (action == KAD_BACKWARD) {
		if (q->to_back)
			for (i = 0; i < n; ++i)
				if (q->x[i] > 0.0f)
					q->g[i] += p->g[i];
	}
	return 0;
}

int kad_op_softmax(kad_node_t *p, int action)
{
	int i, j;
	kad_node_t *q = p->child[0].p;
	assert(q->n_d == 2);
	if (action == KAD_SYNC_DIM) {
		kad_sync_dim1(p, q);
	} else if (action == KAD_FORWARD) {
		float t1 = p->n_child >= 2 && p->child[1].p->x? 1.0f / *p->child[1].p->x : 1.0f;
		for (j = 0; j < p->d[0]; ++j) {
			float *x0, *x, s;
			x0 = q->x + j * p->d[1];
			x = p->x + j * p->d[1];
			for (i = 0, s = 0.0f; i < p->d[1]; ++i)
				s += (x[i] = expf(x0[i] * t1));
			s = 1.0f / s;
			for (i = 0; i < p->d[1]; ++i) x[i] *= s;
		}
	}
	return 0;
}

/////////// General pooling operator ///////////

int kad_op_avg(kad_node_t *p, int action)
{
	int i, n;
	float tmp;
	kad_node_t *q;

	assert(p->n_child > 0);
	tmp = 1.0f / p->n_child;
	q = p->child[0].p;
	n = kad_len(q);
	if (action == KAD_SYNC_DIM) {
		for (i = 1; i < p->n_child; ++i)
			if (kad_len(p->child[i].p) != n) return -1;
		kad_sync_dim1(p, q);
	} else if (action == KAD_FORWARD) {
		memcpy(p->x, q->x, n * sizeof(float));
		for (i = 1; i < p->n_child; ++i)
			kad_saxpy(n, 1.0f, p->child[i].p->x, p->x);
		for (i = 0; i < n; ++i) p->x[i] *= tmp;
	} else if (action == KAD_BACKWARD) {
		for (i = 0; i < p->n_child; ++i)
			if (p->child[i].p->to_back)
				kad_saxpy(n, tmp, p->g, p->child[i].p->g);
	}
	return 0;
}

int kad_op_max(kad_node_t *p, int action)
{
	int i, n;
	kad_node_t *q = p->child[0].p;
	n = kad_len(q);
	if (action == KAD_SYNC_DIM) {
		int *max_j;
		for (i = 1; i < p->n_child; ++i)
			if (kad_len(p->child[i].p) != n) return -1;
		kad_sync_dim1(p, q);
		max_j = (int*)calloc(n, sizeof(int));
		p->child[0].t = (float*)max_j;
	} else if (action == KAD_FORWARD) {
		int j, *max_j = (int*)p->child[0].t;
		memset(max_j, 0, n * sizeof(int));
		memcpy(p->x, q->x, n * sizeof(float));
		for (j = 1; j < p->n_child; ++j)
			for (i = 0, q = p->child[j].p; i < n; ++i)
				if (q->x[i] > p->x[i]) p->x[i] = q->x[i], max_j[i] = j;
	} else if (action == KAD_BACKWARD) {
		int *max_j = (int*)p->child[0].t;
		for (i = 0; i < n; ++i)
			p->child[max_j[i]].p->g[i] += p->g[i];
	}
	return 0;
}

/////////// 2D convolution ///////////

static float *conv2d_move_1to3(int d[4], const float *x) // convert the NCHW shape to the NHWC shape
{
	int i, j, k, l, n = 1;
	float *y;
	for (i = 0; i < 4; ++i) n *= d[i];
	y = (float*)malloc(n * sizeof(float));
	for (i = 0; i < d[0]; ++i)
		for (j = 0; j < d[1]; ++j)
			for (k = 0; k < d[2]; ++k) {
				int ik = (i * d[2] + k) * d[3], ijk = ((i * d[1] + j) * d[2] + k) * d[3];
				for (l = 0; l < d[3]; ++l)
					y[(ik + l) * d[1] + j] = x[ijk + l];
			}
	return y;
}

static void conv2d_add_3to1(int d[4], const float *y, float *x) // convert the NHWC shape back to NCHW and add to another NCHW-shaped array
{
	int i, j, k, l;
	for (i = 0; i < d[0]; ++i)
		for (j = 0; j < d[1]; ++j)
			for (k = 0; k < d[2]; ++k) {
				int ik = (i * d[2] + k) * d[3], ijk = ((i * d[1] + j) * d[2] + k) * d[3];
				for (l = 0; l < d[3]; ++l)
					x[ijk + l] += y[(ik + l) * d[1] + j];
			}
}

#define process_row_for(_xx, _ww, _yy, _wn, _pn, _stride, _t) do { \
	int j, l; \
	if (_stride > 1) { \
		for (l = 0; l < _wn; ++l) { \
			const float *xl = &_xx[l]; \
			for (j = 0; j < _pn; ++j, xl += _stride) _t[j] = *xl; \
			kad_saxpy(_pn, _ww[l], _t, _yy); \
		} \
	} else for (l = 0; l < _wn; ++l) kad_saxpy(_pn, _ww[l], &_xx[l], _yy); \
} while (0)

#define process_row_back_x(_xx, _ww, _yy, _wn, _pn, _stride, _t) do { \
	int j, l; \
	if (_stride > 1) { \
		for (l = 0; l < _wn; ++l) { \
			float *xl = &_xx[l]; \
			memset(_t, 0, _pn * sizeof(float)); \
			kad_saxpy(_pn, _ww[l], _yy, _t); \
			for (j = 0; j < _pn; ++j, xl += _stride) *xl += _t[j]; \
		} \
	} else for (l = 0; l < _wn; ++l) kad_saxpy(_pn, _ww[l], _yy, &_xx[l]); \
} while (0)

#define process_row_back_w(_xx, _ww, _yy, _wn, _pn, _stride, _t) do { \
	int j, l; \
	if (_stride > 1) { \
		for (l = 0; l < _wn; ++l) { \
			const float *xl = &_xx[l]; \
			for (j = 0; j < _pn; ++j, xl += _stride) _t[j] = *xl; \
			_ww[l] += kad_sdot(_pn, _yy, _t); \
		} \
	} else for (l = 0; l < _wn; ++l) _ww[l] += kad_sdot(_pn, _yy, &_xx[l]); \
} while (0)

/* Forward and backward passes are implemented with two different algorithms.
 * The first is faster for small kernels with few input channels; otherwise the
 * second algorithm is faster. Both algorithms should produce identical
 * results, up to the precision of "float".
 */
int kad_op_conv2d(kad_node_t *p, int action) // in the number-channel-height-width (NCHW) shape
{
#define conv2d_loop1(_x, _w, _y, _tmp, _row_func) do { /* for the NCHW shape */ \
		int n, c1, c0, i, k, ii; \
		for (n = 0; n < q->d[0]; ++n) /* mini-batch */ \
			for (c1 = 0; c1 < w->d[0]; ++c1) /* output channel */ \
				for (c0 = 0; c0 < w->d[1]; ++c0) /* input channel */ \
					for (k = 0; k < w->d[2]; ++k) { /* kernel row */ \
						float *_ww = &(_w)[((c1 * w->d[1] + c0) * w->d[2] + k) * w->d[3]]; \
						for (i = 0, ii = k; i < p->d[2]; ++i, ii += aux->r_stride) { /* output row */ \
							float *_xx = &(_x)[((n * q->d[1] + c0) * q->d[2] + ii) * q->d[3]]; \
							float *_yy = &(_y)[((n * p->d[1] + c1) * p->d[2] + i)  * p->d[3]]; \
							_row_func(_xx, _ww, _yy, w->d[3], p->d[3], aux->c_stride, (_tmp)); \
						} /* ~i */ \
					} /* ~k, c0, c1, n */ \
	} while (0)

#define conv2d_loop2(_x, _w, _y, _code) do { /* for the NHWC shape */ \
		int n, c1, i, j, k, ii, j_skip = aux->c_stride * q->d[1], m = w->d[3] * w->d[1]; \
		for (n = 0; n < q->d[0]; ++n) /* mini-batch */ \
			for (c1 = 0; c1 < w->d[0]; ++c1) /* output channel */ \
				for (k = 0; k < w->d[2]; ++k) { /* kernel row */ \
					float *_ww = &(_w)[(c1 * w->d[2] + k) * m]; \
					for (i = 0, ii = k; i < p->d[2]; ++i, ii += aux->r_stride) { /* output and input row */ \
						float *_xx = &(_x)[(n * q->d[2] + ii) * q->d[3] * q->d[1]]; \
						float *_yy = &(_y)[((n * p->d[1] + c1) * p->d[2] + i) * p->d[3]]; \
						for (j = 0; j < p->d[3]; ++j, _xx += j_skip, ++_yy) _code; /* output and input column */ \
					} /* ~i */ \
				} /* ~k, c1, n */ \
	} while (0)

	static const int batch_thres = 16; // use the first algoritm if (num_input_channels * kernel_width) is below this threshold
	kad_conv2d_t *aux = (kad_conv2d_t*)p->ptr;
	kad_node_t *q, *w;

	assert(aux->r_pad == 0 && aux->c_pad == 0); // TODO: padding not implemented yet
	assert(p->n_child == 2);
	q = p->child[0].p, w = p->child[1].p;

	if (action == KAD_SYNC_DIM) {
		if (q->n_d != 4 || w->n_d != 4) return -1;
		if (q->d[1] != w->d[1]) return -1; // unmatched input channels
		if ((q->d[2] - w->d[2] + 2 * aux->r_pad) % aux->r_stride != 0) return -1;
		if ((q->d[3] - w->d[3] + 2 * aux->c_pad) % aux->c_stride != 0) return -1;
		if (aux->r_stride <= aux->r_pad) return -1;
		if (aux->c_stride <= aux->c_pad) return -1;
		p->n_d = 4;
		p->d[0] = q->d[0], p->d[1] = w->d[0];
		p->d[2] = (q->d[2] - w->d[2] + 2 * aux->r_pad) / aux->r_stride + 1;
		p->d[3] = (q->d[3] - w->d[3] + 2 * aux->c_pad) / aux->c_stride + 1;
	} else if (action == KAD_FORWARD) {
		if (w->d[3] * w->d[1] < batch_thres) { // this is the first algorithm
			float *t;
			t = (float*)malloc(p->d[3] * sizeof(float));
			memset(p->x, 0, kad_len(p) * sizeof(float));
			conv2d_loop1(q->x, w->x, p->x, t, process_row_for);
			free(t);
		} else { // this is the second algorithm
			float *q1, *w1;
			memset(p->x, 0, kad_len(p) * sizeof(float));
			q1 = conv2d_move_1to3(q->d, q->x);
			w1 = conv2d_move_1to3(w->d, w->x);
			conv2d_loop2(q1, w1, p->x, (*_yy += kad_sdot(m, _ww, _xx)));
			free(w1); free(q1);
		}
	} else if (action == KAD_BACKWARD) {
		float *t, *q1, *w1;
		// backprop to the input array
		if (p->child[0].p->to_back && w->d[3] * w->d[1] < batch_thres) {
			t = (float*)malloc(p->d[3] * sizeof(float));
			conv2d_loop1(q->g, w->x, p->g, t, process_row_back_x);
			free(t);
		} else if (p->child[0].p->to_back) {
			q1 = (float*)calloc(kad_len(q), sizeof(float));
			w1 = conv2d_move_1to3(w->d, w->x);
			conv2d_loop2(q1, w1, p->g, kad_saxpy(m, *_yy, _ww, _xx));
			conv2d_add_3to1(q->d, q1, q->g);
			free(w1); free(q1);
		}
		// backprop to the weight matrix
		if (p->child[1].p->to_back && w->d[3] * w->d[1] < batch_thres) {
			t = (float*)malloc(p->d[3] * sizeof(float));
			conv2d_loop1(q->x, w->g, p->g, t, process_row_back_w);
			free(t);
		} else if (p->child[1].p->to_back) {
			float *q1, *w1;
			q1 = conv2d_move_1to3(q->d, q->x);
			w1 = (float*)calloc(kad_len(w), sizeof(float));
			conv2d_loop2(q1, w1, p->g, kad_saxpy(m, *_yy, _xx, _ww));
			conv2d_add_3to1(w->d, w1, w->g);
			free(w1); free(q1);
		}
	}
	return 0;
}

int kad_op_max2d(kad_node_t *p, int action)
{
	kad_conv2d_t *aux = (kad_conv2d_t*)p->ptr;
	kad_node_t *q;

	assert(aux->c_pad == 0); // TODO: padding not implemented yet
	q = p->child[0].p;
	if (action == KAD_SYNC_DIM) {
		if (q->n_d != 4) return -1;
		if ((q->d[2] - aux->kernel_h + 2 * aux->r_pad) % aux->r_stride != 0) return -1;
		if ((q->d[3] - aux->kernel_w + 2 * aux->c_pad) % aux->c_stride != 0) return -1;
		if (aux->r_stride <= aux->r_pad) return -1;
		if (aux->c_stride <= aux->c_pad) return -1;
		p->n_d = 4;
		p->d[0] = q->d[0], p->d[1] = q->d[1];
		p->d[2] = (q->d[2] - aux->kernel_h + 2 * aux->r_pad) / aux->r_stride + 1;
		p->d[3] = (q->d[3] - aux->kernel_w + 2 * aux->c_pad) / aux->c_stride + 1;
	} else if (action == KAD_ALLOC) {
		p->child[0].t = (float*)realloc(p->child[0].t, kad_len(p) * sizeof(int));
	} else if (action == KAD_FORWARD) {
		int rest = 1, len, t, i;
		int *f = (int*)p->child[0].t;
		len = kad_len(p);
		for (i = 0; i < len; ++i) p->x[i] = -FLT_MAX;
		for (i = 0; i < p->n_d - 2; ++i) rest *= p->d[i];
		for (t = 0; t < rest; ++t) {
			int i, j, k, l, p_row = p->d[p->n_d - 2], p_col = p->d[p->n_d - 1];
			for (i = 0; i < p_row; ++i) {
				int k_st = i? 0 : aux->r_pad;
				int k_en = i < p_row - 1? aux->kernel_h : aux->kernel_h - aux->r_pad;
				int u = (t * p_row + i) * p_col;
				for (k = k_st; k < k_en; ++k) {
					int v, v0 = (t * q->d[p->n_d - 2] + i * aux->r_stride - aux->r_pad + k) * q->d[p->n_d - 1];
					for (l = 0; l < aux->kernel_w; ++l)
						for (j = 0, v = v0 + l; j < p_col; ++j, v += aux->c_stride)
							if (p->x[u + j] < q->x[v])
								p->x[u + j] = q->x[v], f[u + j] = v;
				} // ~k
			} // ~i
		}
	} else if (action == KAD_BACKWARD) {
		int i, len, *f = (int*)p->child[0].t;
		len = kad_len(p);
		for (i = 0; i < len; ++i) q->g[f[i]] += p->g[i];
	}
	return 0;
}

/////////// 1D convolution ///////////

static float *conv1d_move_1to2(int d[3], const float *x)
{
	float *y;
	int i, j, k;
	y = (float*)malloc(d[0] * d[1] * d[2] * sizeof(float));
	for (k = 0; k < d[0]; ++k)
		for (j = 0; j < d[1]; ++j)
			for (i = 0; i < d[2]; ++i)
				y[(k * d[2] + i) * d[1] + j] = x[(k * d[1] + j) * d[2] + i];
	return y;
}

static void conv1d_add_2to1(int d[3], const float *y, float *x)
{
	int i, j, k;
	for (k = 0; k < d[0]; ++k)
		for (j = 0; j < d[1]; ++j)
			for (i = 0; i < d[2]; ++i)
				x[(k * d[1] + j) * d[2] + i] += y[(k * d[2] + i) * d[1] + j];
}

int kad_op_conv1d(kad_node_t *p, int action) // in the number-channel-width (NCW) shape
{
#define conv1d_loop1(_x, _w, _y, _tmp, _row_func) do { /* for the NCW shape */ \
		int n, c1, c0; \
		for (n = 0; n < q->d[0]; ++n) /* mini-batch */ \
			for (c1 = 0; c1 < w->d[0]; ++c1) /* output channel */ \
				for (c0 = 0; c0 < w->d[1]; ++c0) { /* input channel */ \
					float *_ww = &(_w)[(c1 * w->d[1] + c0) * w->d[2]]; \
					float *_xx = &(_x)[(n  * q->d[1] + c0) * q->d[2]]; \
					float *_yy = &(_y)[(c1 * p->d[1] + c1) * p->d[2]]; \
					_row_func(_xx, _ww, _yy, w->d[2], p->d[2], aux->stride, (_tmp)); \
				} /* ~c0, c1, n */ \
	} while (0)

#define conv1d_loop2(_x, _w, _y, _code) do { /* for the NWC shape */ \
		int n, c1, j, j_skip = aux->stride * q->d[1], m = w->d[2] * w->d[1]; \
		for (n = 0; n < q->d[0]; ++n) /* mini-batch */ \
			for (c1 = 0; c1 < w->d[0]; ++c1) { /* output channel */ \
				float *_ww = &(_w)[c1 * m]; \
				float *_xx = &(_x)[n * q->d[1] * q->d[2]]; \
				float *_yy = &(_y)[((n * p->d[1]) + c1) * p->d[2]]; \
				for (j = 0; j < p->d[2]; ++j, _xx += j_skip, ++_yy) _code; \
			} /* ~c1, n */ \
	} while (0)

	static const int batch_thres = 32; // use the first algoritm if (num_input_channels * kernel_width) is below this threshold
	kad_conv1d_t *aux = (kad_conv1d_t*)p->ptr;
	kad_node_t *q, *w;

	assert(aux->pad == 0); // TODO: padding not implemented yet
	assert(p->n_child == 2);
	q = p->child[0].p, w = p->child[1].p;

	if (action == KAD_SYNC_DIM) {
		if (q->n_d != 3 || w->n_d != 3) return -1;
		if (q->d[1] != w->d[1]) return -1; // unmatched input channels
		if ((q->d[2] - w->d[2] + 2 * aux->pad) % aux->stride != 0) return -1;
		if (aux->stride <= aux->pad) return -1;
		p->n_d = 3;
		p->d[0] = q->d[0], p->d[1] = w->d[0];
		p->d[2] = (q->d[2] - w->d[2] + 2 * aux->pad) / aux->stride + 1;
	} else if (action == KAD_FORWARD) {
		if (w->d[2] * w->d[1] < batch_thres) { // this is the first algorithm
			float *t;
			t = (float*)malloc(p->d[2] * sizeof(float));
			memset(p->x, 0, kad_len(p) * sizeof(float));
			conv1d_loop1(q->x, w->x, p->x, t, process_row_for);
			free(t);
		} else { // this is the second algorithm
			float *q1, *w1;
			memset(p->x, 0, kad_len(p) * sizeof(float));
			q1 = conv1d_move_1to2(q->d, q->x);
			w1 = conv1d_move_1to2(w->d, w->x);
			conv1d_loop2(q1, w1, p->x, (*_yy += kad_sdot(m, _ww, _xx)));
			free(w1); free(q1);
		}
	} else if (action == KAD_BACKWARD) {
		float *t, *q1, *w1;
		// backprop to the input array
		if (p->child[0].p->to_back && w->d[2] * w->d[1] < batch_thres) {
			t = (float*)malloc(p->d[2] * sizeof(float));
			conv1d_loop1(q->g, w->x, p->g, t, process_row_back_x);
			free(t);
		} else if (p->child[0].p->to_back) {
			q1 = (float*)calloc(kad_len(q), sizeof(float));
			w1 = conv1d_move_1to2(w->d, w->x);
			conv1d_loop2(q1, w1, p->g, kad_saxpy(m, *_yy, _ww, _xx));
			conv1d_add_2to1(q->d, q1, q->g);
			free(w1); free(q1);
		}
		// backprop to the weight matrix
		if (p->child[1].p->to_back && w->d[2] * w->d[1] < batch_thres) {
			t = (float*)malloc(p->d[2] * sizeof(float));
			conv1d_loop1(q->x, w->g, p->g, t, process_row_back_w);
			free(t);
		} else if (p->child[1].p->to_back) {
			float *q1, *w1;
			q1 = conv1d_move_1to2(q->d, q->x);
			w1 = (float*)calloc(kad_len(w), sizeof(float));
			conv1d_loop2(q1, w1, p->g, kad_saxpy(m, *_yy, _xx, _ww));
			conv1d_add_2to1(w->d, w1, w->g);
			free(w1); free(q1);
		}
	}
	return 0;
}

int kad_op_max1d(kad_node_t *p, int action)
{
	kad_conv1d_t *aux = (kad_conv1d_t*)p->ptr;
	kad_node_t *q;

	assert(aux->pad == 0); // TODO: padding not implemented yet
	q = p->child[0].p;
	if (action == KAD_SYNC_DIM) {
		if (q->n_d != 3) return -1;
		if ((q->d[2] - aux->kernel_size + 2 * aux->pad) % aux->stride != 0) return -1;
		if (aux->stride <= aux->pad) return -1;
		p->n_d = 3;
		p->d[0] = q->d[0], p->d[1] = q->d[1];
		p->d[2] = (q->d[2] - aux->kernel_size + 2 * aux->pad) / aux->stride + 1;
	} else if (action == KAD_ALLOC) {
		p->child[0].t = (float*)realloc(p->child[0].t, kad_len(p) * sizeof(int));
	} else if (action == KAD_FORWARD) {
		int rest = 1, len, t, i;
		int *f = (int*)p->child[0].t;
		len = kad_len(p);
		for (i = 0; i < len; ++i) p->x[i] = -FLT_MAX;
		for (i = 0; i < p->n_d - 1; ++i) rest *= p->d[i];
		for (t = 0; t < rest; ++t) {
			int j, l, p_width = p->d[p->n_d - 1];
			int u = t * p_width, v, v0 = t * q->d[p->n_d - 1];
			for (l = 0; l < aux->kernel_size; ++l)
				for (j = 0, v = v0 + l; j < p_width; ++j, v += aux->stride)
					if (p->x[u + j] < q->x[v])
						p->x[u + j] = q->x[v], f[u + j] = v;
		}
	} else if (action == KAD_BACKWARD) {
		int i, len, *f = (int*)p->child[0].t;
		len = kad_len(p);
		for (i = 0; i < len; ++i) q->g[f[i]] += p->g[i];
	}
	return 0;
}

/////////// List of operators ///////////

kad_op_f kad_op_list[KAD_MAX_OP] = {
	0,
	kad_op_add,     // 1:  element-wise addition
	kad_op_mul,     // 2:  element-wise multiplication
	kad_op_cmul,    // 3:  column multiplication
	kad_op_ceb,     // 4:  binary cross-entroy for sigmoid activation
	kad_op_norm2,   // 5:  L2-norm
	kad_op_sigm,    // 6:  sigmoid
	kad_op_tanh,    // 7:  tanh
	kad_op_relu,    // 8:  ReLU
	0,
	kad_op_avg,     // 10: general average pooling (not for ConvNet)
	kad_op_1minus,  // 11: 1-x
	kad_op_cem,     // 12: multi-class cross-entropy for softmax activation
	kad_op_softmax, // 13: softmax without temperature
	kad_op_softmax, // 14: softmax with temperature
	kad_op_dropout, // 15: dropout
	kad_op_conv2d,  // 16: 2D convolution
	kad_op_max2d,   // 17: 2D max pooling (for 2D ConvNet)
	kad_op_conv1d,  // 18: 1D convolution
	kad_op_max1d,   // 19: 1D max pooling (for 1D ConvNet)
	kad_op_split,   // 20: split data at a dimension
	kad_op_max      // 21: general max pooling
};

/**************************
 *** Debugging routines ***
 **************************/

void kad_trap_fe(void)
{
#ifdef __SSE__
	_MM_SET_EXCEPTION_MASK(_MM_GET_EXCEPTION_MASK() & ~(_MM_MASK_INVALID | _MM_MASK_DIV_ZERO));
#endif
}

void kad_print_graph(FILE *fp, int n, kad_node_t **v)
{
	static const char *op[] = { 0, "add", "mul", "cmul", "ceb", "norm2", "sigm", "tanh", "relu", 0, "avg", "1minus", "cem", "softmax2", "softmax",
								"dropout", "conv2d", "max2d", "conv1d", "max1d", "subset", "max" };
	int i, j;
	for (i = 0; i < n; ++i) v[i]->tmp = i;
	for (i = 0; i < n; ++i) {
		kad_node_t *p = v[i];
		fprintf(stderr, "%d\t%d\t", i, p->label);
		if (p->pre) fprintf(fp, "%d\t", p->pre->tmp);
		else fprintf(fp, ".\t");
		fputs("[", fp);
		for (j = 0; j < p->n_d; ++j) {
			if (j) fputc(',', fp);
			fprintf(fp, "%d", p->d[j]);
		}
		fprintf(fp, "]\t");
		if (p->n_child) {
			fprintf(fp, "%s(", op[p->op]);
			for (j = 0; j < p->n_child; ++j) {
				if (j) fputc(',', fp);
				fprintf(fp, "$%d", p->child[j].p->tmp);
			}
			fprintf(fp, ")");
		} else fprintf(fp, "%s", p->to_back? "var" : "par");
		fputc('\n', fp);
	}
	for (i = 0; i < n; ++i) v[i]->tmp = 0;
}

static void kad_add_delta(int n, kad_node_t **a, float c, float *delta)
{
	int i, k;
	for (i = k = 0; i < n; ++i)
		if (kad_is_var(a[i])) {
			kad_saxpy(kad_len(a[i]), c, &delta[k], a[i]->x);
			k += kad_len(a[i]);
		}
}

void kad_check_grad(int n, kad_node_t **a, int from)
{
	const float eps = 1e-5, rel = 1e-7 / eps;
	int i, k, n_var;
	float *g0, *delta, f0, f_minus, f_plus, s0, s1, rel_err, p_m_err;
	n_var = kad_n_var(n, a);
	g0 = (float*)calloc(n_var, sizeof(float));
	f0 = *kad_eval_from(n, a, from);
	kad_grad(n, a, from);
	for (i = k = 0; i < n; ++i)
		if (kad_is_var(a[i])) {
			memcpy(&g0[k], a[i]->g, kad_len(a[i]) * sizeof(float));
			k += kad_len(a[i]);
		}
	delta = (float*)calloc(n_var, sizeof(float));
	for (k = 0; k < n_var; ++k) delta[k] = drand48() * eps;
	kad_add_delta(n, a, 1.0f, delta);
	f_plus = *kad_eval_from(n, a, from);
	kad_add_delta(n, a, -2.0f, delta);
	f_minus = *kad_eval_from(n, a, from);
	kad_add_delta(n, a, 1.0f, delta);
	s0 = kad_sdot(n_var, g0, delta);
	s1 = .5 * (f_plus - f_minus);
	fprintf(stderr, "Gradient check -- %g <=> %g @ %g -- ", s0/eps, s1/eps, f0);
	if (fabs(s1) >= rel * eps) {
		rel_err = fabs(fabs(s0) - fabs(s1)) / (fabs(s0) + fabs(s1));
		p_m_err = fabs(f_plus + f_minus - 2.0f * f0) / fabs(f_plus - f_minus);
		fprintf(stderr, "rel_err:%g p_m_err:%g -- ", rel_err, p_m_err);
		if (rel_err >= rel && rel_err > p_m_err) fprintf(stderr, "failed\n");
		else fprintf(stderr, "passed\n");
	} else fprintf(stderr, "skipped\n");
	free(delta); free(g0);
}
