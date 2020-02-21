#pragma once

#include <color_spinor_field.h>
#include <color_spinor_field_order.h>
#include <dslash_quda.h>
#include <index_helper.cuh>
#include <inline_ptx.h>
#include <math_helper.cuh>
#include <shared_memory_cache_helper.cuh>

#if (__COMPUTE_CAPABILITY__ >= 700)
#include <mma.h>
#endif

namespace quda
{

#if defined(GPU_DOMAIN_WALL_DIRAC) && (__COMPUTE_CAPABILITY__ >= 700)

  template <class T> struct TensorCoreSharedMemory {
    __device__ inline operator T *()
    {
      extern __shared__ int __smem[];
      return (T *)__smem;
    }

    __device__ inline operator const T *() const
    {
      extern __shared__ int __smem[];
      return (T *)__smem;
    }
  };

  // matrix a for a generic matrix: column major, M/M_sm(size/padded size) by k
  // (spin,Ls) by (spin,Ls), where left most index is the fastest changing
  // one(spin).
  // x by y
  // For now, assuming it's trivial in spin
  template <int block_dim_x, int Ls_, int M_sm, class compute_type>
  __device__ inline void construct_matrix_a_generic(half *sm_a, compute_type *generic)
  {

    int offset_k = threadIdx.y * 4;
    int x = threadIdx.x;

    while (x < Ls_) {
      int offset_m = x * 4;
      float value = generic[x * Ls_ + threadIdx.y]; // Assuming the input matrix is row major

      // exp = 0 means we are on the diagonal.
      sm_a[(offset_k + 0) * (M_sm) + (offset_m + 0)] = value;
      sm_a[(offset_k + 1) * (M_sm) + (offset_m + 1)] = value;
      sm_a[(offset_k + 2) * (M_sm) + (offset_m + 2)] = value;
      sm_a[(offset_k + 3) * (M_sm) + (offset_m + 3)] = value;

      // sm_a[ (offset_k+0)*(M_sm)+(offset_m+0) ] = factorR + factorL;
      sm_a[(offset_k + 0) * (M_sm) + (offset_m + 1)] = static_cast<half>(0.0f);
      sm_a[(offset_k + 0) * (M_sm) + (offset_m + 2)] = static_cast<half>(0.0f);
      sm_a[(offset_k + 0) * (M_sm) + (offset_m + 3)] = static_cast<half>(0.0f);

      sm_a[(offset_k + 1) * (M_sm) + (offset_m + 0)] = static_cast<half>(0.0f);
      // sm_a[ (offset_k+1)*(M_sm)+(offset_m+1) ] = factorR + factorL;
      sm_a[(offset_k + 1) * (M_sm) + (offset_m + 2)] = static_cast<half>(0.0f);
      sm_a[(offset_k + 1) * (M_sm) + (offset_m + 3)] = static_cast<half>(0.0f);

      sm_a[(offset_k + 2) * (M_sm) + (offset_m + 0)] = static_cast<half>(0.0f);
      sm_a[(offset_k + 2) * (M_sm) + (offset_m + 1)] = static_cast<half>(0.0f);
      // sm_a[ (offset_k+2)*(M_sm)+(offset_m+2) ] = factorR + factorL;
      sm_a[(offset_k + 2) * (M_sm) + (offset_m + 3)] = static_cast<half>(0.0f);

      sm_a[(offset_k + 3) * (M_sm) + (offset_m + 0)] = static_cast<half>(0.0f);
      sm_a[(offset_k + 3) * (M_sm) + (offset_m + 1)] = static_cast<half>(0.0f);
      sm_a[(offset_k + 3) * (M_sm) + (offset_m + 2)] = static_cast<half>(0.0f);
      // sm_a[ (offset_k+3)*(M_sm)+(offset_m+3) ] = factorR + factorL;

      x += block_dim_x;
    }
  }

  // matrix a for m5inv: column major, M/M_sm(size/padded size) by k
  // (spin,Ls) by (spin,Ls), where left most index is the fastest changing
  // one(spin).
  // x by y
  template <int block_dim_x, int Ls_, int M_sm, bool dagger, class Arg>
  __device__ inline void construct_matrix_a_m5inv(Arg &arg, half *sm_a, const float *mp = nullptr,
                                                  const float *mm = nullptr)
  {
    const float k = arg.kappa;
    // if we rescale, then the actual matrix is alpha*m5inv+beta.
    // Otherwise a = 1., b = 0.;
    const float b = arg.beta;

    const float inv = arg.alpha * arg.fac_inv;

    int offset_k = threadIdx.y * 4;
    int x = threadIdx.x;

    while (x < Ls_) {
      int offset_m = x * 2;
      float factorR, factorL;

      if (mp && mm) {
        if (dagger) {
          factorR = mp[x * Ls_ + threadIdx.y];
          factorL = mm[x * Ls_ + threadIdx.y];
        } else {
          factorR = mp[threadIdx.y * Ls_ + x];
          factorL = mm[threadIdx.y * Ls_ + x];
        }
      } else {
        int exp;
        if (dagger) {
          exp = x > threadIdx.y ? Ls_ - x + threadIdx.y : threadIdx.y - x;
          factorR = inv * powf(k, __int2float_rn(exp)) * (x > threadIdx.y ? -arg.m_f : 1.f);
        } else {
          exp = x < threadIdx.y ? Ls_ - threadIdx.y + x : x - threadIdx.y;
          factorR = inv * powf(k, __int2float_rn(exp)) * (x < threadIdx.y ? -arg.m_f : 1.f);
        }

        if (dagger) {
          exp = x < threadIdx.y ? Ls_ - threadIdx.y + x : x - threadIdx.y;
          factorL = inv * powf(k, __int2float_rn(exp)) * (x < threadIdx.y ? -arg.m_f : 1.f);
        } else {
          exp = x > threadIdx.y ? Ls_ - x + threadIdx.y : threadIdx.y - x;
          factorL = inv * powf(k, __int2float_rn(exp)) * (x > threadIdx.y ? -arg.m_f : 1.f);
        }
      }

      float RpL = x == threadIdx.y ? factorR + factorL + b : factorR + factorL;
      float RmL = factorR - factorL;

      half2 *A = reinterpret_cast<half2 *>(sm_a);

      A[(offset_k + 0) * (M_sm / 2) + (offset_m + 0)] = __floats2half2_rn(RpL, 0.0f);
      A[(offset_k + 0) * (M_sm / 2) + (offset_m + 1)] = __floats2half2_rn(RmL, 0.0f);

      A[(offset_k + 1) * (M_sm / 2) + (offset_m + 0)] = __floats2half2_rn(0.0f, RpL);
      A[(offset_k + 1) * (M_sm / 2) + (offset_m + 1)] = __floats2half2_rn(0.0f, RmL);

      A[(offset_k + 2) * (M_sm / 2) + (offset_m + 0)] = __floats2half2_rn(RmL, 0.0f);
      A[(offset_k + 2) * (M_sm / 2) + (offset_m + 1)] = __floats2half2_rn(RpL, 0.0f);

      A[(offset_k + 3) * (M_sm / 2) + (offset_m + 0)] = __floats2half2_rn(0.0f, RmL);
      A[(offset_k + 3) * (M_sm / 2) + (offset_m + 1)] = __floats2half2_rn(0.0f, RpL);

      x += block_dim_x;
    }
  }

  // matrix a for m5pre: column major, M/M_sm(size/padded size) by k
  // (spin,Ls) by (spin,Ls), where left most index is the fastest changing
  // one(spin).
  // x by y
  template <int block_dim_x, int Ls_, int M_sm, bool dagger, class Arg>
  __device__ inline void construct_matrix_a_d5(Arg &arg, half *sm_a)
  {
    // if we rescale, then the actual matrix is alpha*m5inv+beta.
    // Otherwise a = 1., b = 0.;
    const float b = arg.beta;

    int offset_k = threadIdx.y * 4;
    int x = threadIdx.x;

    while (x < Ls_) {
      int offset_m = x * 2;
      int exp = x - threadIdx.y;
      float factorR, factorL;

      if (dagger) {
        factorR = (exp == -1 ? 1.f : (exp == +Ls_ - 1 ? -arg.m_f : 0.f));
      } else {
        factorR = (exp == +1 ? 1.f : (exp == -Ls_ + 1 ? -arg.m_f : 0.f));
      }

      if (dagger) {
        factorL = (exp == +1 ? 1.f : (exp == -Ls_ + 1 ? -arg.m_f : 0.f));
      } else {
        factorL = (exp == -1 ? 1.f : (exp == +Ls_ - 1 ? -arg.m_f : 0.f));
      }

      // exp = 0 means we are on the diagonal.
      float RpL = exp == 0 ? arg.alpha * (factorR + factorL) + b : arg.alpha * (factorR + factorL);
      float RmL = arg.alpha * (factorR - factorL);

      half2 *A = reinterpret_cast<half2 *>(sm_a);

      A[(offset_k + 0) * (M_sm / 2) + (offset_m + 0)] = __floats2half2_rn(RpL, 0.0f);
      A[(offset_k + 0) * (M_sm / 2) + (offset_m + 1)] = __floats2half2_rn(RmL, 0.0f);

      A[(offset_k + 1) * (M_sm / 2) + (offset_m + 0)] = __floats2half2_rn(0.0f, RpL);
      A[(offset_k + 1) * (M_sm / 2) + (offset_m + 1)] = __floats2half2_rn(0.0f, RmL);

      A[(offset_k + 2) * (M_sm / 2) + (offset_m + 0)] = __floats2half2_rn(RmL, 0.0f);
      A[(offset_k + 2) * (M_sm / 2) + (offset_m + 1)] = __floats2half2_rn(RpL, 0.0f);

      A[(offset_k + 3) * (M_sm / 2) + (offset_m + 0)] = __floats2half2_rn(0.0f, RmL);
      A[(offset_k + 3) * (M_sm / 2) + (offset_m + 1)] = __floats2half2_rn(0.0f, RpL);

      x += block_dim_x;
    }
  }

  template <class integer_vec> __device__ inline integer_vec __2half22integer4_rn(const half2 &a, const half2 &b)
  {
    integer_vec c;
    c.x = __half2short_rn(a.x);
    c.y = __half2short_rn(a.y);
    c.z = __half2short_rn(b.x);
    c.w = __half2short_rn(b.y);
    return c;
  }

  template <class integer_vec>
  __device__ inline integer_vec __4half22integer8_rn(const half2 &a, const half2 &b, const half2 &c, const half2 &d)
  {
    integer_vec e;
    e.x.x = __half2short_rn(a.x);
    e.x.y = __half2short_rn(a.y);
    e.x.z = __half2short_rn(b.x);
    e.x.w = __half2short_rn(b.y);
    e.y.x = __half2short_rn(c.x);
    e.y.y = __half2short_rn(c.y);
    e.y.z = __half2short_rn(d.x);
    e.y.w = __half2short_rn(d.y);
    return e;
  }

  __device__ inline void __half_max_abs_half2__(half &max, const half2 &input)
  {
    // Set the fisrt bit of the halves to 0.
    static constexpr uint32_t maximum_mask = 0x7fff7fffu; // 0111 1111 1111 1111 0111 1111 1111 1111

    uint32_t input_masked = *reinterpret_cast<const uint32_t *>(&input) & maximum_mask;
    half2 lh = *reinterpret_cast<half2 *>(&input_masked);
    if (__hgt(lh.x, max)) { max = lh.x; }
    if (__hgt(lh.y, max)) { max = lh.y; }
  }

  __inline__ __device__ void __float_max_abs_floats__(float &max, const float &input)
  {
    static constexpr uint32_t maximum_mask = 0x7fffffffu; // 0111 1111 1111 1111 1111 1111 1111 1111
    uint32_t input_masked = *reinterpret_cast<const uint32_t *>(&input) & maximum_mask;
    if (*reinterpret_cast<float *>(&input_masked) > max) { max = *reinterpret_cast<float *>(&input_masked); }
  }

  __device__ inline void warp_wise_reduce_float(float &f)
  {
#pragma unroll
    for (int offset = 16; offset > 0; offset /= 2) {
      // TODO: Only works for CUDA 9.2 or later
      float other_f = __shfl_down_sync(0xffffffffu, f, offset);
      if (other_f > f) { f = other_f; }
    }
  }

  constexpr float target_scale = 2e3;

  template <class Vector> __device__ inline void block_wise_reduce_vector(const Vector &v, float *smem_scale)
  {

    __syncthreads();

    int lane_id = ((threadIdx.y * blockDim.x + threadIdx.x) & 31);
    int warp_id = ((threadIdx.y * blockDim.x + threadIdx.x) >> 5);

    // Find the maximum absolute value in a lane
    float warp_max = 0.0f;
#pragma unroll
    for (int spin = 0; spin < 4; spin++) {
#pragma unroll
      for (int color = 0; color < 3; color++) {
        __float_max_abs_floats__(warp_max, v(spin, color).real());
        __float_max_abs_floats__(warp_max, v(spin, color).imag());
      }
    }
    // Find the maximum absolute value in a warp across different lanes
    warp_wise_reduce_float(warp_max);
    // Now lane 0 of each warp holds the maximum value
    if (lane_id == 0) { smem_scale[warp_id] = warp_max; }
    __syncthreads();
    if (warp_id == 0) {
      warp_max = (lane_id < ((blockDim.x * blockDim.y) >> 5)) ? smem_scale[lane_id] : 0.0f;
      warp_wise_reduce_float(warp_max);
      if (lane_id == 0) { smem_scale[0] = warp_max / target_scale; }
    }
    __syncthreads();
  }

  // Actually does more than the function name suggests.
  // will find the maximum absolute value among the vector, scale that, and store
  // to sm_b
  template <int N_sm_d2, bool accumulate, bool store = true, class Vector>
  __device__ inline void load_matrix_b_vector(Vector &v, half2 *sm_b, float *smem_scale, float m_scale = 1.0f)
  {
    if (accumulate) {
      float previous_scale = smem_scale[0] * m_scale;
      half *B = reinterpret_cast<half *>(sm_b);
#pragma unroll
      for (int spin = 0; spin < 4; spin++) {
#pragma unroll
        for (int color = 0; color < 3; color++) {
          int idx = (threadIdx.y * 4 + spin) * (N_sm_d2 * 2) + 6 * threadIdx.x + 2 * color;
          v(spin, color) += complex<float>(__half2float(B[idx + 0]), __half2float(B[idx + 1])) * previous_scale;
        }
      }
    }
    if (store) {
      block_wise_reduce_vector(v, smem_scale);
      // Now smem_scale[0] contains the maximum value
      float current_scale = smem_scale[0];
#pragma unroll
      for (int spin = 0; spin < 4; spin++) {
#pragma unroll
        for (int color = 0; color < 3; color++) {
          float real = v(spin, color).real() / current_scale;
          float imag = v(spin, color).imag() / current_scale;
          int idx = (threadIdx.y * 4 + spin) * N_sm_d2 + 3 * threadIdx.x + color;
          sm_b[idx] = __floats2half2_rn(real, imag);
        }
      }
    }
  }

  // Store results(scaled short/char values and scale) in shared memroy to global
  // memroy.
  template <class storage_type, int N_sm, class Output>
  __device__ inline void store_matrix_c(Output &output, half2 *sm_b, int sid, const float scale)
  {
    half max_ = 0.0f;
    constexpr int N_sm_d2 = N_sm / 2;
#pragma unroll
    for (int spin = 0; spin < 4; spin++) {
#pragma unroll
      for (int color = 0; color < 3; color++) {
        int idx = (threadIdx.y * 4 + spin) * N_sm_d2 + 3 * threadIdx.x + color;
        __half_max_abs_half2__(max_, sm_b[idx]);
      }
    }

    output.norm[sid] = __half2float(max_) * scale;

    const half2 max_i_div_max2_ = __half2half2(__hdiv(fixedMaxValue<storage_type>::value, max_));
#ifdef FLOAT8 // use float8/short8
    typedef typename VectorType<storage_type, 8>::type storage_vec;
    storage_vec *out = reinterpret_cast<storage_vec *>(output.field);
    half2 a, b, c, d;

    a = __hmul2(sm_b[(threadIdx.y * 4 + 0) * N_sm_d2 + 3 * threadIdx.x + 0], max_i_div_max2_);
    b = __hmul2(sm_b[(threadIdx.y * 4 + 0) * N_sm_d2 + 3 * threadIdx.x + 1], max_i_div_max2_);
    c = __hmul2(sm_b[(threadIdx.y * 4 + 0) * N_sm_d2 + 3 * threadIdx.x + 2], max_i_div_max2_);
    d = __hmul2(sm_b[(threadIdx.y * 4 + 1) * N_sm_d2 + 3 * threadIdx.x + 0], max_i_div_max2_);
    vector_store(&out[sid + 0 * output.volumeCB], 0, __4half22integer8_rn<storage_vec>(a, b, c, d));

    a = __hmul2(sm_b[(threadIdx.y * 4 + 1) * N_sm_d2 + 3 * threadIdx.x + 1], max_i_div_max2_);
    b = __hmul2(sm_b[(threadIdx.y * 4 + 1) * N_sm_d2 + 3 * threadIdx.x + 2], max_i_div_max2_);
    c = __hmul2(sm_b[(threadIdx.y * 4 + 2) * N_sm_d2 + 3 * threadIdx.x + 0], max_i_div_max2_);
    d = __hmul2(sm_b[(threadIdx.y * 4 + 2) * N_sm_d2 + 3 * threadIdx.x + 1], max_i_div_max2_);
    vector_store(&out[sid + 1 * output.volumeCB], 0, __4half22integer8_rn<storage_vec>(a, b, c, d));

    a = __hmul2(sm_b[(threadIdx.y * 4 + 2) * N_sm_d2 + 3 * threadIdx.x + 2], max_i_div_max2_);
    b = __hmul2(sm_b[(threadIdx.y * 4 + 3) * N_sm_d2 + 3 * threadIdx.x + 0], max_i_div_max2_);
    c = __hmul2(sm_b[(threadIdx.y * 4 + 3) * N_sm_d2 + 3 * threadIdx.x + 1], max_i_div_max2_);
    d = __hmul2(sm_b[(threadIdx.y * 4 + 3) * N_sm_d2 + 3 * threadIdx.x + 2], max_i_div_max2_);
    vector_store(&out[sid + 2 * output.volumeCB], 0, __4half22integer8_rn<storage_vec>(a, b, c, d));
#else

    typedef typename VectorType<storage_type, 4>::type storage_vec;
    storage_vec *out = reinterpret_cast<storage_vec *>(output.field);
    half2 a, b;

    a = __hmul2(sm_b[(threadIdx.y * 4 + 0) * N_sm_d2 + 3 * threadIdx.x + 0], max_i_div_max2_);
    b = __hmul2(sm_b[(threadIdx.y * 4 + 0) * N_sm_d2 + 3 * threadIdx.x + 1], max_i_div_max2_);
    out[sid + 0 * output.volumeCB] = __2half22integer4_rn<storage_vec>(a, b);

    a = __hmul2(sm_b[(threadIdx.y * 4 + 0) * N_sm_d2 + 3 * threadIdx.x + 2], max_i_div_max2_);
    b = __hmul2(sm_b[(threadIdx.y * 4 + 1) * N_sm_d2 + 3 * threadIdx.x + 0], max_i_div_max2_);
    out[sid + 1 * output.volumeCB] = __2half22integer4_rn<storage_vec>(a, b);

    a = __hmul2(sm_b[(threadIdx.y * 4 + 1) * N_sm_d2 + 3 * threadIdx.x + 1], max_i_div_max2_);
    b = __hmul2(sm_b[(threadIdx.y * 4 + 1) * N_sm_d2 + 3 * threadIdx.x + 2], max_i_div_max2_);
    out[sid + 2 * output.volumeCB] = __2half22integer4_rn<storage_vec>(a, b);

    a = __hmul2(sm_b[(threadIdx.y * 4 + 2) * N_sm_d2 + 3 * threadIdx.x + 0], max_i_div_max2_);
    b = __hmul2(sm_b[(threadIdx.y * 4 + 2) * N_sm_d2 + 3 * threadIdx.x + 1], max_i_div_max2_);
    out[sid + 3 * output.volumeCB] = __2half22integer4_rn<storage_vec>(a, b);

    a = __hmul2(sm_b[(threadIdx.y * 4 + 2) * N_sm_d2 + 3 * threadIdx.x + 2], max_i_div_max2_);
    b = __hmul2(sm_b[(threadIdx.y * 4 + 3) * N_sm_d2 + 3 * threadIdx.x + 0], max_i_div_max2_);
    out[sid + 4 * output.volumeCB] = __2half22integer4_rn<storage_vec>(a, b);

    a = __hmul2(sm_b[(threadIdx.y * 4 + 3) * N_sm_d2 + 3 * threadIdx.x + 1], max_i_div_max2_);
    b = __hmul2(sm_b[(threadIdx.y * 4 + 3) * N_sm_d2 + 3 * threadIdx.x + 2], max_i_div_max2_);
    out[sid + 5 * output.volumeCB] = __2half22integer4_rn<storage_vec>(a, b);

#endif
  }

  // For "reload" version(reload == true) of wmma gemm, matrix a is loaded when
  // needed.
  // It is a waste of time but has less register usage.
  // For "preload" version(reload == false) of wmma gemm, matrix a is preloaded
  // before hand.
  // It saves time but uses more registers.
  template <int block_dim_x, int Ls_, int M, int N, int M_sm, int N_sm, bool reload, class T>
  __device__ inline void wmma_gemm(T *a_frag, half *sm_a, half *sm_b, half *sm_c)
  {
    constexpr int WMMA_M = 16;
    constexpr int WMMA_N = 16;
    constexpr int WMMA_K = 16;

    constexpr int tm_dim = M / WMMA_M;
    constexpr int tn_dim = N / WMMA_N;

    constexpr int total_warp = block_dim_x * Ls_ / 32;

    static_assert((tm_dim * tn_dim) % total_warp == 0, "(tm_dim*tn_dim)%%total_warp==0\n");
    static_assert(tn_dim % (tm_dim * tn_dim / total_warp) == 0, "tn_dim%%(tm_dim*tn_dim/total_warp)==0\n");

    const int this_warp = (threadIdx.y * block_dim_x + threadIdx.x) >> 5;

    constexpr int total_tile = tm_dim * tn_dim;

    constexpr int warp_cycle = total_tile / total_warp;
    const int warp_m = this_warp * warp_cycle / tn_dim;
#pragma unroll
    for (int c = 0; c < warp_cycle; c++) {
      // Set up the wmma stuff
      nvcuda::wmma::fragment<nvcuda::wmma::matrix_b, WMMA_M, WMMA_N, WMMA_K, half, nvcuda::wmma::row_major> b_frag;
      nvcuda::wmma::fragment<nvcuda::wmma::accumulator, WMMA_M, WMMA_N, WMMA_K, half> c_frag;

      // The logical warp assigned to each part of the matrix.
      const int phys_warp_index = this_warp * warp_cycle + c;
      const int warp_n = phys_warp_index - warp_m * tn_dim;
      // eg. for 12 warps:
      // 000|111|222|333
      // 444|555|666|777
      // 888|999|000|111

      // Zero the initial acc.
      nvcuda::wmma::fill_fragment(c_frag, static_cast<half>(0.0f));

#pragma unroll
      for (int k = 0; k < tm_dim; k++) {
        const int a_row = warp_m * WMMA_M;
        const int a_col = k * WMMA_K;
        const int b_row = k * WMMA_K;
        const int b_col = warp_n * WMMA_N;

        // Load Matrix
        if (reload) { nvcuda::wmma::load_matrix_sync(a_frag[0], sm_a + a_row + a_col * M_sm, M_sm); }
        nvcuda::wmma::load_matrix_sync(b_frag, sm_c + b_col + b_row * N_sm, N_sm);
        // Perform the matrix multiplication
        if (reload) {
          nvcuda::wmma::mma_sync(c_frag, a_frag[0], b_frag, c_frag);
        } else {
          nvcuda::wmma::mma_sync(c_frag, a_frag[k], b_frag, c_frag);
        }
      }

      __syncthreads();

      int c_row = warp_m * WMMA_M;
      int c_col = warp_n * WMMA_N;

      nvcuda::wmma::store_matrix_sync(sm_c + c_col + c_row * N_sm, c_frag, N_sm, nvcuda::wmma::mem_row_major);
    }
  }

  template <class data_type, class smem_access_type>
  __device__ inline void mma_load_a_frag(smem_access_type ra[], const data_type *smem_a, const int reg_offset,
                                         const int smem_offset)
  {
    const smem_access_type *A = reinterpret_cast<const smem_access_type *>(smem_a);
    ra[reg_offset + 0] = A[smem_offset + 0];
    ra[reg_offset + 1] = A[smem_offset + 1];
  }

  // #define USE_FP16_MMA_ACCUMULATE

  template <int BlockDimX, int Ls, int M, int N, int M_PAD, int N_PAD>
  __device__ inline void mma_sync_gemm(half *sm_a, half *sm_b, half *sm_c)
  {
    using data_type = half;
    using smem_access_type = unsigned;
    constexpr int data_pack_factor = sizeof(unsigned) / sizeof(data_type);

    constexpr int WMMA_M = 16; // WMMA_M == WMMA_K
    constexpr int WMMA_N = 16;
#ifdef USE_FP16_MMA_ACCUMULATE
    constexpr bool fp16_accumulate = true;
    using accumuate_reg_type = unsigned;
#else
    constexpr bool fp16_accumulate = false;
    using accumuate_reg_type = float;
#endif
    constexpr int accumuate_regs = fp16_accumulate ? 4 : 8;

    constexpr int tile_row_dim = M / WMMA_M; // number of tiles in the column dimension
    constexpr int tile_col_dim = N / WMMA_N; // number of tiles in the row dimension

    constexpr int total_warp = BlockDimX * Ls / 32;

    static_assert((tile_row_dim * tile_col_dim) % total_warp == 0,
                  "Total number of tiles should be divisible by the number of warps.");
    static_assert(tile_col_dim % (tile_row_dim * tile_col_dim / total_warp) == 0,
                  "Each warp should only be responsible a single tile row.");

    constexpr int total_tile = tile_row_dim * tile_col_dim;

    constexpr int warp_cycle = total_tile / total_warp;

    const int thread_id = threadIdx.y * blockDim.x + threadIdx.x;
    const int warp_id = thread_id >> 5;
    const int warp_row = warp_id * warp_cycle / tile_col_dim;

    const int lane_id = thread_id & 31;
    const int octl_id = lane_id >> 2;
    const int quad_id = octl_id & 3;
    const int quad_row = quad_id & 1;
    const int quad_col = quad_id >> 1;
    const int quad_hilo = (octl_id >> 2) & 1; // quad higher or lower.
    const int quad_thread = lane_id & 3;      // 0,1,2,3

    smem_access_type ra[2 * tile_row_dim * 4];

#pragma unroll
    for (int c = 0; c < warp_cycle; c++) {

      accumuate_reg_type rc[accumuate_regs];
#pragma unroll
      for (int r = 0; r < accumuate_regs; r++) { rc[r] = 0; }

      // The logical warp assigned to each part of the matrix.
      const int logical_warp_index = warp_id * warp_cycle + c;
      const int warp_col = logical_warp_index - warp_row * tile_col_dim;
      // e.g. for 12 warps:
      // 000|111|222|333
      // 444|555|666|777
      // 888|999|000|111

#pragma unroll
      for (int tile_k = 0; tile_k < tile_row_dim; tile_k++) {
#pragma unroll
        for (int warp_k = 0; warp_k < 4; warp_k++) {

          smem_access_type *B = reinterpret_cast<smem_access_type *>(sm_b);

          const int k_idx = (tile_k * 4 + warp_k) * 2;
          const int idx_strided = k_idx * 2 + quad_thread;

          int thread_offset_b = idx_strided * (N_PAD / data_pack_factor) + warp_col * 8 + quad_col * 4 + quad_hilo * 2;

          if (c == 0) { // the data in registers can be resued.
            const int thread_offset_a
              = idx_strided * (M_PAD / data_pack_factor) + warp_row * 8 + quad_row * 4 + quad_hilo * 2;
            mma_load_a_frag(ra, sm_a, k_idx, thread_offset_a);
          }
          unsigned rb[2];
          rb[0] = B[thread_offset_b + 0];
          rb[1] = B[thread_offset_b + 1];

#ifdef USE_FP16_MMA_ACCUMULATE
          asm volatile("mma.sync.aligned.m8n8k4.col.row.f16.f16.f16.f16 {%0,%1,%2,%3}, {%4,%5}, {%6,%7}, {%0,%1,%2,%3};"
                       : "+r"(rc[0]), "+r"(rc[1]), "+r"(rc[2]), "+r"(rc[3])
                       : "r"(ra[k_idx + 0]), "r"(ra[k_idx + 1]), "r"(rb[0]), "r"(rb[1]));
#else
          asm volatile("mma.sync.aligned.m8n8k4.col.row.f32.f16.f16.f32 {%0,%1,%2,%3,%4,%5,%6,%7}, {%8,%9}, {%10,%11}, "
                       "{%0,%1,%2,%3,%4,%5,%6,%7};"
                       : "+f"(rc[0]), "+f"(rc[1]), "+f"(rc[2]), "+f"(rc[3]), "+f"(rc[4]), "+f"(rc[5]), "+f"(rc[6]),
                         "+f"(rc[7])
                       : "r"(ra[k_idx + 0]), "r"(ra[k_idx + 1]), "r"(rb[0]), "r"(rb[1]));
#endif
        }
      }

      __syncthreads();
#ifdef USE_FP16_MMA_ACCUMULATE
      smem_access_type *C = reinterpret_cast<smem_access_type *>(sm_c);
      const int row = warp_row * 16 + quad_row * 8 + quad_hilo * 4 + quad_thread;
      const int col = warp_col * 8 + quad_col * 4;
      const int thread_offset_c = row * (N_PAD / data_pack_factor) + col;
#pragma unroll
      for (int i = 0; i < 4; i++) { C[thread_offset_c + i] = rc[i]; }
#else
      half2 *C = reinterpret_cast<half2 *>(sm_c);

      const int row = warp_row * 16 + quad_row * 8 + quad_hilo * 4 + (quad_thread % 2);
      const int col = warp_col * 8 + quad_col * 4 + (quad_thread / 2);

      int thread_offset_c = row * (N_PAD / data_pack_factor) + col;
      C[thread_offset_c] = __floats2half2_rn(rc[0], rc[1]);

      thread_offset_c = (row + 2) * (N_PAD / data_pack_factor) + col;
      C[thread_offset_c] = __floats2half2_rn(rc[2], rc[3]);

      thread_offset_c = row * (N_PAD / data_pack_factor) + (col + 2);
      C[thread_offset_c] = __floats2half2_rn(rc[4], rc[5]);

      thread_offset_c = (row + 2) * (N_PAD / data_pack_factor) + (col + 2);
      C[thread_offset_c] = __floats2half2_rn(rc[6], rc[7]);
#endif
    }
  }

#endif // defined (GPU_DOMAIN_WALL_DIRAC) && (__COMPUTE_CAPABILITY__ >= 700)

} // namespace quda
