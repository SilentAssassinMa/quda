#include <copy_color_spinor_mg.cuh>

namespace quda {
  
  void copyGenericColorSpinorMGQH(ColorSpinorField &dst, const ColorSpinorField &src, 
				  QudaFieldLocation location, void *Dst, void *Src, 
				  void *dstNorm, void *srcNorm) {

#if defined(GPU_MULTIGRID) && (QUDA_PRECISION & 2) && (QUDA_PRECISION & 1)
    char *dst_ptr = static_cast<char*>(Dst);
    short *src_ptr = static_cast<short*>(Src);

    INSTANTIATE_COLOR;
#else
    errorQuda("Half and quarter precision have not been enabled");
#endif

  }

} // namespace quda
