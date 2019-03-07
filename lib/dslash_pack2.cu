#ifndef USE_LEGACY_DSLASH

#include <typeinfo>
#include <color_spinor_field.h>
#include <color_spinor_field_order.h>
#include <color_spinor.h>

#define STRIPED
#ifdef STRIPED
#else
#define SWIZZLE // needs to be before index_helper is included
#endif

#include <dslash.h>
#include <index_helper.cuh>
#include <dslash_quda.h>

namespace quda {

  static int commDim[QUDA_MAX_DIM];
  void setPackComms(const int *comm_dim) {
    for (int i=0; i<4; i++) commDim[i] = comm_dim[i];
    for (int i=4; i<QUDA_MAX_DIM; i++) commDim[i] = 0;
  }

  template <typename Float_, int nColor_, int nSpin_>
  struct PackArg {

    typedef Float_ Float;
    typedef typename mapper<Float>::type real;

    static constexpr int nColor = nColor_;
    static constexpr int nSpin = nSpin_;

    static constexpr bool spin_project = (nSpin == 4 ? true : false);
    static constexpr bool spinor_direct_load = false; // false means texture load
    typedef typename colorspinor_mapper<Float,nSpin,nColor,spin_project,spinor_direct_load>::type F;

    const F field;     // field we are packing

    const int nFace;

    const bool dagger;

    const int parity;  // only use this for single parity fields
    const int nParity;  // number of parities we are working on
    const QudaPCType pc_type; // preconditioning type (4-d or 5-d)

    const DslashConstant dc; // pre-computed dslash constants for optimized indexing

    real a; // preconditioned twisted-mass scaling parameter
    real b; // preconditioned twisted-mass chiral twist factor
    real c; // preconditioned twisted-mass flavor twist factor
    int twist; // whether we are doing preconditioned twisted-mass or not (1 - singlet, 2 - doublet)

    int_fastdiv threads;
    int threadDimMapLower[4];
    int threadDimMapUpper[4];

    int_fastdiv swizzle;
    int sites_per_block;

    PackArg(void **ghost, const ColorSpinorField &field, int nFace, bool dagger, int parity,
            int threads, double a, double b, double c) :
        field(field, nFace, nullptr, nullptr, reinterpret_cast<Float **>(ghost)),
        nFace(nFace),
        dagger(dagger),
        parity(parity),
        nParity(field.SiteSubset()),
        threads(threads),
        pc_type(field.PCType()),
        dc(field.getDslashConstant()),
        a(a),
        b(b),
        c(c),
        twist((a != 0.0 && b != 0.0) ? (c != 0.0 ? 2 : 1) : 0)
    {
      if (!field.isNative()) errorQuda("Unsupported field order colorspinor=%d\n", field.FieldOrder());

      int prev = -1; // previous dimension that was partitioned
      for (int i=0; i<4; i++) {
        threadDimMapLower[i] = 0;
        threadDimMapUpper[i] = 0;
        if (!commDim[i]) continue;
        threadDimMapLower[i] = (prev >= 0 ? threadDimMapUpper[prev] : 0);
        threadDimMapUpper[i] = threadDimMapLower[i] + 2*nFace*dc.ghostFaceCB[i];
        prev=i;
      }
    }

  };

  template <typename Float, int nSpin, int nColor>
  std::ostream& operator<<(std::ostream& out, const PackArg<Float,nSpin,nColor> &arg)
  {
    out << "parity = " << arg.parity << std::endl;
    out << "nParity = " << arg.nParity << std::endl;
    out << "pc_type = " << arg.pc_type << std::endl;
    out << "nFace = " << arg.nFace << std::endl;
    out << "dagger = " << arg.dagger << std::endl;
    out << "a = " << arg.a << std::endl;
    out << "b = " << arg.b << std::endl;
    out << "c = " << arg.c << std::endl;
    out << "twist = " << arg.twist << std::endl;
    out << "threads = " << arg.threads << std::endl;
    out << "threadDimMapLower = { ";
    for (int i=0; i<4; i++) out << arg.threadDimMapLower[i] << (i<3 ? ", " : " }"); out << std::endl;
    out << "threadDimMapUpper = { ";
    for (int i=0; i<4; i++) out << arg.threadDimMapUpper[i] << (i<3 ? ", " : " }"); out << std::endl;
    out << "sites_per_block = " << arg.sites_per_block << std::endl;
    return out;
  }

  template <bool dagger, int twist, int dim, QudaPCType pc, typename Arg>
  __device__ __host__ inline void pack(Arg &arg, int ghost_idx, int s, int parity) {

    typedef typename mapper<typename Arg::Float>::type real;
    typedef ColorSpinor<real,Arg::nColor,Arg::nSpin> Vector;
    constexpr int nFace = 1;

    // this means we treat 4-d preconditioned fields as 4-d fields,
    // and don't fold in any fifth dimension until after we have
    // computed the 4-d indices (saves division)
    constexpr int nDim = pc;

    // for 5-d preconditioning the face_size includes the Ls dimension
    const int face_size = nFace*arg.dc.ghostFaceCB[dim] * (pc == QUDA_5D_PC ? arg.dc.Ls : 1);

    int spinor_parity = (arg.nParity == 2) ? parity : 0;

    // compute where the output is located
    // compute an index into the local volume from the index into the face
    // read spinor, spin-project, and write half spinor to face

    // face_num determines which end of the lattice we are packing: 0 = start, 1 = end
    const int face_num = (ghost_idx >= face_size) ? 1 : 0;
    ghost_idx -= face_num*face_size;

    // remove const to ensure we have non-const Ghost member
    typedef typename std::remove_const<decltype(arg.field)>::type T;
    T &field = const_cast<T&>(arg.field);

    if (face_num == 0) { // backwards

      int idx = indexFromFaceIndex<nDim,pc,dim,nFace,0>(ghost_idx, parity, arg);
      constexpr int proj_dir = dagger ? +1 : -1;
      Vector f = arg.field(idx+s*arg.dc.volume_4d_cb, spinor_parity);
      if (twist==1) {
        f = arg.a * (f + arg.b*f.igamma(4));
      } else if (twist==2) {
        Vector f1 = arg.field(idx+(1-s)*arg.dc.volume_4d_cb, spinor_parity); // load other flavor
        if (s==0) f = arg.a * (f + arg.b*f.igamma(4) + arg.c*f1);
        else      f = arg.a * (f - arg.b*f.igamma(4) + arg.c*f1);
      }
      field.Ghost(dim, 0, ghost_idx+s*arg.dc.ghostFaceCB[dim], spinor_parity) = f.project(dim, proj_dir);

    } else { // forwards

      int idx = indexFromFaceIndex<nDim,pc,dim,nFace,1>(ghost_idx, parity, arg);
      constexpr int proj_dir = dagger ? -1 : +1;
      Vector f = arg.field(idx+s*arg.dc.volume_4d_cb, spinor_parity);
      if (twist==1) {
        f = arg.a * (f + arg.b*f.igamma(4));
      } else if (twist==2) {
        Vector f1 = arg.field(idx+(1-s)*arg.dc.volume_4d_cb, spinor_parity); // load other flavor
        if (s==0) f = arg.a * (f + arg.b*f.igamma(4) + arg.c*f1);
        else      f = arg.a * (f - arg.b*f.igamma(4) + arg.c*f1);
      }
      field.Ghost(dim, 1, ghost_idx+s*arg.dc.ghostFaceCB[dim], spinor_parity) = f.project(dim, proj_dir);
    }
  }

  template <int dim, typename Arg, int nFace = 1>
  __device__ __host__ inline void packStaggered(Arg &arg, int ghost_idx, int s, int parity)
  {
    typedef typename mapper<typename Arg::Float>::type real;
    typedef ColorSpinor<real, Arg::nColor, Arg::nSpin> Vector;

    int spinor_parity = (arg.nParity == 2) ? parity : 0;

    // compute where the output is located
    // compute an index into the local volume from the index into the face
    // read spinor, spin-project, and write half spinor to face

    // face_num determines which end of the lattice we are packing: 0 = start, 1 = end
    const int face_num = (ghost_idx >= nFace * arg.dc.ghostFaceCB[dim]) ? 1 : 0;
    ghost_idx -= face_num * nFace * arg.dc.ghostFaceCB[dim];

    // remove const to ensure we have non-const Ghost member
    typedef typename std::remove_const<decltype(arg.field)>::type T;
    T &field = const_cast<T &>(arg.field);

    if (face_num == 0) { // backwards
      int idx = indexFromFaceIndexStaggered<4, QUDA_4D_PC, dim, nFace, 0>(ghost_idx, parity, arg);
      Vector f = arg.field(idx + s * arg.dc.volume_4d_cb, spinor_parity);
      field.Ghost(dim, 0, ghost_idx + s * arg.dc.ghostFaceCB[dim], spinor_parity) = f;
    } else {                                                                           // forwards
      int idx = indexFromFaceIndexStaggered<4, QUDA_4D_PC, dim, nFace, 1>(ghost_idx, parity, arg);
      Vector f = arg.field(idx + s * arg.dc.volume_4d_cb, spinor_parity);
      field.Ghost(dim, 1, ghost_idx + s * arg.dc.ghostFaceCB[dim], spinor_parity) = f;
    }
  }

  // FIXME - add CPU variant

  template <bool dagger, int twist, QudaPCType pc, typename Arg>
  __global__ void packKernel(Arg arg)
  {

#ifdef STRIPED
    const int sites_per_block = arg.sites_per_block;
    int local_tid = threadIdx.x;
    int tid = sites_per_block * blockIdx.x + local_tid;
#else
    int tid = block_idx(arg.swizzle) * blockDim.x + threadIdx.x;
    constexpr int sites_per_block = 1;
    constexpr int local_tid = 0;
#endif
    int s = blockDim.y*blockIdx.y + threadIdx.y;
    if (s >= arg.dc.Ls) return;

    // this is the parity used for load/store, but we use arg.parity for index mapping
    int parity = (arg.nParity == 2) ? blockDim.z*blockIdx.z + threadIdx.z : arg.parity;

    while ( local_tid < sites_per_block && tid < arg.threads ) {

      // determine which dimension we are packing
      int ghost_idx;
      const int dim = dimFromFaceIndex(ghost_idx, tid, arg);

      if (pc == QUDA_5D_PC) { // 5-d checkerboarded, include s (not ghostFaceCB since both faces)
        switch (dim) {
        case 0: pack<dagger,twist,0,pc>(arg, ghost_idx+s*arg.dc.ghostFace[0], 0, parity); break;
        case 1: pack<dagger,twist,1,pc>(arg, ghost_idx+s*arg.dc.ghostFace[1], 0, parity); break;
        case 2: pack<dagger,twist,2,pc>(arg, ghost_idx+s*arg.dc.ghostFace[2], 0, parity); break;
        case 3: pack<dagger,twist,3,pc>(arg, ghost_idx+s*arg.dc.ghostFace[3], 0, parity); break;
        }
      } else { // 4-d checkerboarding, keeping s separate (if it exists)
        switch (dim) {
        case 0: pack<dagger,twist,0,pc>(arg, ghost_idx, s, parity); break;
        case 1: pack<dagger,twist,1,pc>(arg, ghost_idx, s, parity); break;
        case 2: pack<dagger,twist,2,pc>(arg, ghost_idx, s, parity); break;
        case 3: pack<dagger,twist,3,pc>(arg, ghost_idx, s, parity); break;
        }
      }

#ifdef STRIPED
      local_tid += blockDim.x;
      tid += blockDim.x;
#else
      tid += blockDim.x * gridDim.x;
#endif
    } // while tid
  }

  template <typename Arg>
  __global__ void packStaggeredKernel(Arg arg)
  {

#ifdef STRIPED
    const int sites_per_block = arg.sites_per_block;
    int local_tid = threadIdx.x;
    int tid = sites_per_block * blockIdx.x + local_tid;
#else
    int tid = block_idx(arg.swizzle) * blockDim.x + threadIdx.x;
    constexpr int sites_per_block = 1;
    constexpr int local_tid = 0;
#endif
    int s = blockDim.y * blockIdx.y + threadIdx.y;
    if (s >= arg.dc.Ls) return;

    // this is the parity used for load/store, but we use arg.parity for index mapping
    int parity = (arg.nParity == 2) ? blockDim.z * blockIdx.z + threadIdx.z : arg.parity;

    while (local_tid < sites_per_block && tid < arg.threads) {

      // determine which dimension we are packing
      int ghost_idx;
      const int dim = dimFromFaceIndex(ghost_idx, tid, arg);

      if (arg.nFace == 1) {
        switch (dim) {
        case 0: packStaggered<0, Arg, 1>(arg, ghost_idx, s, parity); break;
        case 1: packStaggered<1, Arg, 1>(arg, ghost_idx, s, parity); break;
        case 2: packStaggered<2, Arg, 1>(arg, ghost_idx, s, parity); break;
        case 3: packStaggered<3, Arg, 1>(arg, ghost_idx, s, parity); break;
        }
      } else if (arg.nFace == 3) {
        switch (dim) {
        case 0: packStaggered<0, Arg, 3>(arg, ghost_idx, s, parity); break;
        case 1: packStaggered<1, Arg, 3>(arg, ghost_idx, s, parity); break;
        case 2: packStaggered<2, Arg, 3>(arg, ghost_idx, s, parity); break;
        case 3: packStaggered<3, Arg, 3>(arg, ghost_idx, s, parity); break;
        }
      }

#ifdef STRIPED
      local_tid += blockDim.x;
      tid += blockDim.x;
#else
      tid += blockDim.x*gridDim.x;
#endif
    } // while tid
  }

  template <typename Float, int nColor>
  class Pack : TunableVectorYZ {

  protected:
    void **ghost;
    const ColorSpinorField &field;
    MemoryLocation location;
    const int nFace;
    const bool dagger; // only has meaning for nSpin=4
    const int parity;
    const int nParity;
    int threads;
    const double a;
    const double b;
    const double c;
    int twist;         // only has meaning for nSpin=4

#ifdef STRIPED
    bool tuneGridDim() const { return true; } // If striping, always tune grid dimension
    unsigned int maxGridSize() const
    {
      if (location & Host) {
	// if zero-copy policy then set a maximum number of blocks to be
	// the 3 * number of dimensions we are communicating
        int nDimComms = 0;
        for (int d=0; d<field.Ndim(); d++) nDimComms += commDim[d];
        return 3*nDimComms;
      } else {
        return TunableVectorYZ::maxGridSize();
      }
    } // use no more than a quarter of the GPU
    unsigned int minGridSize() const
    {
      if (location & Host) {
	// if zero-copy policy then set a maximum number of blocks to be
	// the 1 * number of dimensions we are communicating
        int nDimComms = 0;
        for (int d=0; d<field.Ndim(); d++) nDimComms += commDim[d];
        return nDimComms;
      } else {
        return TunableVectorYZ::minGridSize();
      }
    }
#else
    bool tuneGridDim() const { return location & Host; } // only tune grid dimension if doing zero-copy writing
    unsigned int maxGridSize() const
    {
      return tuneGridDim() ? deviceProp.multiProcessorCount/4 : TunableVectorYZ::maxGridSize();
    } // use no more than a quarter of the GPU
#endif

    bool tuneAuxDim() const { return true; } // Do tune the aux dimensions.
    unsigned int minThreads() const { return threads; }

    void fillAux()
    {
      strcpy(aux,"policy_kernel,");
      strcat(aux, field.AuxString());
      char comm[5];
      for (int i=0; i<4; i++) comm[i] = (commDim[i] ? '1' : '0');
      comm[4] = '\0';
      strcat(aux,",comm=");
      strcat(aux,comm);
      strcat(aux,comm_dim_topology_string());
      if (field.PCType() == QUDA_5D_PC) { strcat(aux,",5D_pc"); }
      if (dagger && field.Nspin() == 4) { strcat(aux, ",dagger"); }
      if (getKernelPackT()) { strcat(aux,",kernelPackT"); }
      switch (nFace) {
      case 1: strcat(aux,",nFace=1"); break;
      case 3: strcat(aux,",nFace=3"); break;
      default: errorQuda("Number of faces not supported");
      }

      twist = ((a != 0.0 && b != 0.0) ? (c != 0.0 ? 2 : 1) : 0);
      if (twist) strcat(aux, twist==2 ? ",twist-doublet," : ",twist-singlet,");

      // label the locations we are packing to
      // location lable is nonp2p-p2p
      switch ((int)location) {
      case Device|Remote: strcat(aux,"device-remote"); break;
      case   Host|Remote: strcat(aux,  "host-remote"); break;
      case        Device: strcat(aux,"device-device"); break;
      case          Host: strcat(aux, comm_peer2peer_enabled_global() ? "host-device" : "host-host"); break;
      default: errorQuda("Unknown pack target location %d\n", location);
      }
    }

  public:

    Pack(void *ghost[], const ColorSpinorField &field, MemoryLocation location, int nFace,
         bool dagger, int parity, double a , double b, double c) :
      TunableVectorYZ( (field.Ndim() == 5 ? field.X(4) : 1), field.SiteSubset()),
      ghost(ghost),
      field(field),
      location(location),
      nFace(nFace),
      dagger(dagger),
      parity(parity),
      nParity(field.SiteSubset()),
      threads(0),
      a(a),
      b(b),
      c(c)
    {
      fillAux();

      // compute number of threads - really number of active work items we have to do
      for (int i=0; i<4; i++) {
        if (!commDim[i]) continue;
        if ( i==3 && !getKernelPackT() ) continue;
        threads += 2*nFace*field.getDslashConstant().ghostFaceCB[i]; // 2 for forwards and backwards faces
      }
    }

    virtual ~Pack() { }

    void apply(const cudaStream_t &stream)
    {
      TuneParam tp = tuneLaunch(*this, getTuning(), getVerbosity());

      if (field.Nspin() == 4) {
        PackArg<Float,nColor,4> arg(ghost, field, nFace, dagger, parity, threads, a, b, c);
        arg.swizzle = tp.aux.x;
        arg.sites_per_block = (arg.threads + tp.grid.x - 1) / tp.grid.x;

        if (field.PCType() == QUDA_4D_PC) {
          if (arg.dagger) {
            switch(arg.twist) {
            case 0: packKernel<true, 0, QUDA_4D_PC> <<<tp.grid,tp.block,tp.shared_bytes,stream>>>(arg); break;
            case 1: packKernel<true, 1, QUDA_4D_PC> <<<tp.grid,tp.block,tp.shared_bytes,stream>>>(arg); break;
            case 2: packKernel<true, 2, QUDA_4D_PC> <<<tp.grid,tp.block,tp.shared_bytes,stream>>>(arg); break;
            }
          } else {
            switch(arg.twist) {
            case 0: packKernel<false, 0, QUDA_4D_PC> <<<tp.grid,tp.block,tp.shared_bytes,stream>>>(arg); break;
            default: errorQuda("Twisted packing only for dagger");
            }
          }
        } else if (arg.pc_type == QUDA_5D_PC) {
          if (arg.twist) errorQuda("Twist packing not defined");
          if (arg.dagger) {
            packKernel<true, 0, QUDA_5D_PC> <<<tp.grid,tp.block,tp.shared_bytes,stream>>>(arg);
          } else {
            packKernel<false, 0, QUDA_5D_PC> <<<tp.grid,tp.block,tp.shared_bytes,stream>>>(arg);
          }
        }
      } else if (field.Nspin() == 1) {
        PackArg<Float,nColor,1> arg(ghost, field, nFace, dagger, parity, threads, a, b, c);
        arg.swizzle = tp.aux.x;
        arg.sites_per_block = (arg.threads + tp.grid.x - 1) / tp.grid.x;
        packStaggeredKernel<<<tp.grid, tp.block, tp.shared_bytes, stream>>>(arg);
      } else {
        errorQuda("Unsupported nSpin = %d\n", field.Nspin());
      }
    }

    bool tuneSharedBytes() const { return false; }

    bool advanceAux(TuneParam &param) const
    {
#ifdef SWIZZLE
      if ( location & Remote ) {  // only swizzling if we're doing remote writing
        if (param.aux.x < (int)maxGridSize()) {
          param.aux.x++;
          return true;
        } else {
          param.aux.x = 1;
          return false;
        }
      } else {
        return false;
      }
#else
      return false;
#endif
    }

    void initTuneParam(TuneParam &param) const
    {
      TunableVectorYZ::initTuneParam(param);
      param.aux.x = 1; // swizzle factor
      // if doing a zero-copy policy then ensure that each thread block
      // runs exclusively on a given SM - this is to ensure quality of
      // service for the packing kernel when running concurrently.
      // FIXME - we could set max shared memory on Volta
      if (location & Host) param.shared_bytes = deviceProp.sharedMemPerBlock / 2 + 1;
    }

    void defaultTuneParam(TuneParam &param) const
    {
      TunableVectorYZ::defaultTuneParam(param);
      param.aux.x = 1; // swizzle factor
    }

    TuneKey tuneKey() const { return TuneKey(field.VolString(), typeid(*this).name(), aux); }

    int tuningIter() const { return 3; }

    long long flops() const
    {
      // unless we are spin projecting (nSpin = 4), there are no flops to do
      return field.Nspin() == 4 ? 2 * field.Nspin()/2 * nColor * nParity * field.getDslashConstant().Ls * threads : 0;
    }

    long long bytes() const
    {
      size_t precision = sizeof(Float);
      size_t faceBytes = 2 * ( (field.Nspin() == 4 ? field.Nspin()/2 : field.Nspin()) + field.Nspin() ) * nColor * precision;
      if (precision == QUDA_HALF_PRECISION || precision == QUDA_QUARTER_PRECISION)
        faceBytes += 2*sizeof(float); // 2 is from input and output
      return faceBytes * nParity * field.getDslashConstant().Ls * threads;
    }

  };

  template <typename Float, int nColor>
  void PackGhost(void *ghost[], const ColorSpinorField &field,
                 MemoryLocation location, int nFace, bool dagger, int parity,
                 double a, double b, double c, const cudaStream_t &stream)
  {
    Pack<Float,nColor> pack(ghost, field, location, nFace, dagger, parity, a, b, c);
    pack.apply(stream);
  }

  // template on the number of colors
  template <typename Float>
  void PackGhost(void *ghost[], const ColorSpinorField &field,
                 MemoryLocation location, int nFace, bool dagger, int parity,
                 double a, double b, double c, const cudaStream_t &stream)
  {
    if (field.Ncolor() == 3) {
      PackGhost<Float,3>(ghost, field, location, nFace, dagger, parity, a, b, c, stream);
    } else {
      errorQuda("Unsupported number of colors %d\n", field.Ncolor());
    }
  }

  // Pack the ghost for the Dslash operator
  void PackGhost(void *ghost[2*QUDA_MAX_DIM], const ColorSpinorField &field,
                 MemoryLocation location, int nFace, bool dagger, int parity,
                 double a, double b, double c, const cudaStream_t &stream)
  {
    int nDimPack = 0;
    for (int d=0; d<4; d++) {
      if (!commDim[d]) continue;
      if (d != 3 || getKernelPackT()) nDimPack++;
    }

    if (!nDimPack) return; // if zero then we have nothing to pack

    if (field.Precision() == QUDA_DOUBLE_PRECISION) {
      PackGhost<double>(ghost, field, location, nFace, dagger, parity, a, b, c, stream);
    } else if (field.Precision() == QUDA_SINGLE_PRECISION) {
      PackGhost<float>(ghost, field, location, nFace, dagger, parity, a, b, c, stream);
    } else if (field.Precision() == QUDA_HALF_PRECISION) {
      PackGhost<short>(ghost, field, location, nFace, dagger, parity, a, b, c, stream);
    } else if (field.Precision() == QUDA_QUARTER_PRECISION) {
      PackGhost<char>(ghost, field, location, nFace, dagger, parity, a, b, c, stream);
    } else {
      errorQuda("Unsupported precision %d\n", field.Precision());
    }
  }


} // namespace quda

#endif // USE_LEGACY_DSLASH