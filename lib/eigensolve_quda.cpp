#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <vector>
#include <algorithm>

#include <invert_quda.h>

#include <quda_internal.h>
#include <eigensolve_quda.h>
#include <qio_field.h>
#include <color_spinor_field.h>
#include <blas_quda.h>
#include <util_quda.h>
#include <sys/time.h>

#include <Eigen/Eigenvalues>
#include <Eigen/Dense>

bool flags = true;

namespace quda
{

  using namespace Eigen;

  // Eigensolver class
  //-----------------------------------------------------------------------------
  EigenSolver::EigenSolver(QudaEigParam *eig_param, TimeProfile &profile) : eig_param(eig_param), profile(profile)
  {

    // Timings for components of the eigensolver
    time_ = 0.0;
    time_e = 0.0;   // time in Eigen
    time_mv = 0.0;  // time in matVec
    time_mb = 0.0;  // time in multiblas
    time_svd = 0.0; // time to compute SVD

    // Problem parameters
    nEv = eig_param->nEv;
    nKr = eig_param->nKr;
    nConv = eig_param->nConv;
    tol = eig_param->tol;
    reverse = false;

    // Algorithm variables
    converged = false;
    restart_iter = 0;
    max_restarts = eig_param->max_restarts;
    check_interval = eig_param->check_interval;
    iter = 0;
    iter_converged = 0;
    iter_locked = 0;
    iter_keep = 0;
    num_converged = 0;
    num_locked = 0;
    num_keep = 0;

    // Sanity checks
    if (nKr <= nEv) errorQuda("nKr=%d is less than or equal to nEv=%d\n", nKr, nEv);
    if (nEv < nConv) errorQuda("nConv=%d is greater than nEv=%d\n", nConv, nEv);
    if (nEv == 0) errorQuda("nEv=0 passed to Eigensolver\n");
    if (nKr == 0) errorQuda("nKr=0 passed to Eigensolver\n");
    if (nConv == 0) errorQuda("nConv=0 passed to Eigensolver\n");

    residua = new double[nKr];
    for (int i = 0; i < nKr; i++) { residua[nKr] = 0.0; }

    // Quda MultiBLAS freindly array
    Qmat = new Complex[nEv * nKr];

    // Part of the spectrum to be computed.
    spectrum = strdup("SR"); // Initialsed to stop the compiler warning.

    if (eig_param->use_poly_acc) {
      if (eig_param->spectrum == QUDA_SPECTRUM_SR_EIG)
        spectrum = strdup("LR");
      else if (eig_param->spectrum == QUDA_SPECTRUM_LR_EIG)
        spectrum = strdup("SR");
      else if (eig_param->spectrum == QUDA_SPECTRUM_SM_EIG)
        spectrum = strdup("LM");
      else if (eig_param->spectrum == QUDA_SPECTRUM_LM_EIG)
        spectrum = strdup("SM");
      else if (eig_param->spectrum == QUDA_SPECTRUM_SI_EIG)
        spectrum = strdup("LI");
      else if (eig_param->spectrum == QUDA_SPECTRUM_LI_EIG)
        spectrum = strdup("SI");
    } else {
      if (eig_param->spectrum == QUDA_SPECTRUM_SR_EIG)
        spectrum = strdup("SR");
      else if (eig_param->spectrum == QUDA_SPECTRUM_LR_EIG)
        spectrum = strdup("LR");
      else if (eig_param->spectrum == QUDA_SPECTRUM_SM_EIG)
        spectrum = strdup("SM");
      else if (eig_param->spectrum == QUDA_SPECTRUM_LM_EIG)
        spectrum = strdup("LM");
      else if (eig_param->spectrum == QUDA_SPECTRUM_SI_EIG)
        spectrum = strdup("SI");
      else if (eig_param->spectrum == QUDA_SPECTRUM_LI_EIG)
        spectrum = strdup("LI");
    }

    // Deduce whether to reverse the sorting
    const char *L = "L";
    const char *S = "S";
    if (strncmp(L, spectrum, 1) == 0 && !eig_param->use_poly_acc) {
      reverse = true;
    } else if (strncmp(S, spectrum, 1) == 0 && eig_param->use_poly_acc) {
      reverse = true;
    } else if (strncmp(L, spectrum, 1) == 0 && eig_param->use_poly_acc) {
      reverse = true;
    }

    // Print Eigensolver params
    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("nConv %d\n", nConv);
      printfQuda("nEv %d\n", nEv);
      printfQuda("nKr %d\n", nKr);
      if (eig_param->use_poly_acc) {
        printfQuda("polyDeg %d\n", eig_param->poly_deg);
        printfQuda("a-min %f\n", eig_param->a_min);
        printfQuda("a-max %f\n", eig_param->a_max);
      }
    }
  }

  // We bake the matrix operator 'mat' and the eigensolver parameters into the
  // eigensolver.
  EigenSolver *EigenSolver::create(QudaEigParam *eig_param, const DiracMatrix &mat, TimeProfile &profile)
  {

    EigenSolver *eig_solver = nullptr;

    switch (eig_param->eig_type) {
    case QUDA_EIG_ARNOLDI: errorQuda("Arnoldi not implemented"); break;
    case QUDA_EIG_LANCZOS:
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Creating TRLM eigensolver\n");
      eig_solver = new TRLM(eig_param, mat, profile);
      break;
    case QUDA_EIG_JD:
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Creating JD eigensolver\n");
      eig_solver = new JD(eig_param, mat, profile);
      break;
    default: errorQuda("Invalid eig solver type");
    }
    return eig_solver;
  }

  // Utilities and functions common to all Eigensolver instances
  //------------------------------------------------------------------------------

  void EigenSolver::matVec(const DiracMatrix &mat, ColorSpinorField &out, const ColorSpinorField &in)
  {
    mat(out, in);
    return;
  }

  void EigenSolver::chebyOp(const DiracMatrix &mat, ColorSpinorField &out, const ColorSpinorField &in)
  {

    // Just do a simple matVec if no poly acc is requested
    if (!eig_param->use_poly_acc) {
      time_ = -clock();
      mat(out, in);
      time_ += clock();
      time_mv += time_;
      return;
    }

    if (eig_param->poly_deg == 0) { errorQuda("Polynomial acceleration requested with zero polynomial degree"); }

    // Compute the polynomial accelerated operator.
    double delta, theta;
    double sigma, sigma1, sigma_old;
    double d1, d2, d3;

    double a = eig_param->a_min;
    double b = eig_param->a_max;

    delta = (b - a) / 2.0;
    theta = (b + a) / 2.0;

    sigma1 = -delta / theta;

    d1 = sigma1 / delta;
    d2 = 1.0;

    // out = d2 * in + d1 * out
    // C_1(x) = x
    time_ = -clock();
    mat(out, in);
    time_ += clock();
    time_mv += time_;

    time_ = -clock();
    blas::caxpby(d2, const_cast<ColorSpinorField &>(in), d1, out);
    if (eig_param->poly_deg == 1) return;

    // C_0 is the current 'in'  vector.
    // C_1 is the current 'out' vector.

    // Clone 'in' to two temporary vectors.
    ColorSpinorField *tmp1 = ColorSpinorField::Create(in);
    ColorSpinorField *tmp2 = ColorSpinorField::Create(in);

    blas::copy(*tmp1, in);
    blas::copy(*tmp2, out);
    time_ += clock();
    time_mb += time_;

    // Using Chebyshev polynomial recursion relation,
    // C_{m+1}(x) = 2*x*C_{m} - C_{m-1}

    sigma_old = sigma1;

    // construct C_{m+1}(x)
    for (int i = 2; i < eig_param->poly_deg; i++) {

      sigma = 1.0 / (2.0 / sigma1 - sigma_old);

      d1 = 2.0 * sigma / delta;
      d2 = -d1 * theta;
      d3 = -sigma * sigma_old;

      // mat*C_{m}(x)
      time_ = -clock();
      mat(out, *tmp2);
      time_ += clock();
      time_mv += time_;

      time_ = -clock();
      blas::ax(d3, *tmp1);
      Complex d1c(d1, 0.0);
      Complex d2c(d2, 0.0);
      blas::cxpaypbz(*tmp1, d2c, *tmp2, d1c, out);

      blas::copy(*tmp1, *tmp2);
      blas::copy(*tmp2, out);
      time_ += clock();
      time_mb += time_;

      sigma_old = sigma;
    }

    delete tmp1;
    delete tmp2;
  }

  // Orthogonalise r against V_[j]
  Complex EigenSolver::orthogonalize(std::vector<ColorSpinorField *> vecs, std::vector<ColorSpinorField *> rvec, int j)
  {
    time_ = -clock();
    Complex s(0.0, 0.0);
    Complex sum(0.0, 0.0);
    for (int i = 0; i < j; i++) {
      s = blas::cDotProduct(*vecs[i], *rvec[0]);
      sum += s;
      blas::caxpy(-s, *vecs[i], *rvec[0]);
    }
    time_ += clock();
    time_mb += time_;
    return sum;
  }

  // Orthogonalise r against V_[j]
  Complex EigenSolver::blockOrthogonalize(std::vector<ColorSpinorField *> vecs, std::vector<ColorSpinorField *> rvec,
                                          int j)
  {
    time_ = -clock();
    Complex *s = new Complex[j + 1];
    Complex sum(0.0, 0.0);
    std::vector<ColorSpinorField *> vecs_ptr;
    for (int i = 0; i < j + 1; i++) { vecs_ptr.push_back(vecs[i]); }
    // Block dot products stored in s.
    blas::cDotProduct(s, vecs_ptr, rvec);

    // Block orthogonalise
    for (int i = 0; i < j + 1; i++) {
      sum += s[i];
      s[i] *= -1.0;
    }
    blas::caxpy(s, vecs_ptr, rvec);

    delete []s;
    time_ += clock();
    time_mb += time_;
    return sum;
  }

  // Deflate vec, place result in vec_defl
  void EigenSolver::deflate(std::vector<ColorSpinorField *> vec_defl, std::vector<ColorSpinorField *> vec,
                            std::vector<ColorSpinorField *> eig_vecs, std::vector<Complex> evals)
  {

    // number of evecs
    int n_defl = eig_param->nEv;

    if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Deflating %d vectors\n", n_defl);

    // Perform Sum_i V_i * (L_i)^{-1} * (V_i)^dag * vec = vec_defl
    // for all i computed eigenvectors and values.

    // Pointers to the required Krylov space vectors,
    // no extra memory is allocated.
    std::vector<ColorSpinorField *> eig_vecs_ptr;
    for (int i = 0; i < n_defl; i++) eig_vecs_ptr.push_back(eig_vecs[i]);

    // 1. Take block inner product: (V_i)^dag * vec = A_i
    Complex *s = new Complex[n_defl];
    blas::cDotProduct(s, eig_vecs_ptr, vec);

    // 2. Perform block caxpy: V_i * (L_i)^{-1} * A_i
    for (int i = 0; i < n_defl; i++) { s[i] /= evals[i].real(); }

    // 3. Accumulate sum vec_defl = Sum_i V_i * (L_i)^{-1} * A_i
    blas::zero(*vec_defl[0]);
    blas::caxpy(s, eig_vecs_ptr, vec_defl);
    // FIXME - we can optimize the zeroing out with a "multi-caxy"
    // function that just writes over vec_defl and doesn't sum.  When
    // we exceed the multi-blas limit this would deompose into caxy
    // for the kernel call and caxpy for the subsequent ones
  }

  void EigenSolver::computeEvals(const DiracMatrix &mat, std::vector<ColorSpinorField *> &evecs,
                                 std::vector<Complex> &evals, int size)
  {
    for (int i = 0; i < size; i++) {

      // r = A * v_i
      time_ = -clock();
      mat(*r[0], *evecs[i]);
      time_ += clock();
      time_mv += time_;

      time_ = -clock();
      // lambda_i = v_i^dag A v_i / (v_i^dag * v_i)
      evals[i] = blas::cDotProduct(*evecs[i], *r[0]) / sqrt(blas::norm2(*evecs[i]));

      // Measure ||lambda_i*v_i - A*v_i||
      Complex n_unit(-1.0, 0.0);
      blas::caxpby(evals[i], *evecs[i], n_unit, *r[0]);
      residua[i] = sqrt(blas::norm2(*r[0]));
      time_ += clock();
      time_mb += time_;
    }
  }

  void EigenSolver::loadVectors(std::vector<ColorSpinorField *> &eig_vecs, std::string vec_infile)
  {
    // profile.TPSTOP(QUDA_PROFILE_COMPUTE);
    // profile.TPSTART(QUDA_PROFILE_IO);

#ifdef HAVE_QIO
    const int Nvec = eig_vecs.size();
    if (strcmp(vec_infile.c_str(), "") != 0) {
      if (getVerbosity() >= QUDA_SUMMARIZE)
        printfQuda("Start loading %04d vectors from %s\n", Nvec, vec_infile.c_str());

      std::vector<ColorSpinorField *> tmp;
      if (eig_vecs[0]->Location() == QUDA_CUDA_FIELD_LOCATION) {
        ColorSpinorParam csParam(*eig_vecs[0]);
        csParam.fieldOrder = QUDA_SPACE_SPIN_COLOR_FIELD_ORDER;
        csParam.setPrecision(eig_vecs[0]->Precision() < QUDA_SINGLE_PRECISION ? QUDA_SINGLE_PRECISION :
                                                                                eig_vecs[0]->Precision());
        csParam.location = QUDA_CPU_FIELD_LOCATION;
        csParam.create = QUDA_NULL_FIELD_CREATE;
        for (int i = 0; i < Nvec; i++) { tmp.push_back(ColorSpinorField::Create(csParam)); }
      } else {
        for (int i = 0; i < Nvec; i++) { tmp.push_back(eig_vecs[i]); }
      }

      void **V = static_cast<void **>(safe_malloc(Nvec * sizeof(void *)));
      for (int i = 0; i < Nvec; i++) {
        V[i] = tmp[i]->V();
        if (V[i] == NULL) {
          if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Could not allocate space for eigenVector[%d]\n", i);
        }
      }

      read_spinor_field(vec_infile.c_str(), &V[0], tmp[0]->Precision(), tmp[0]->X(),
                        tmp[0]->Ncolor(), tmp[0]->Nspin(), Nvec, 0, (char **)0);

      host_free(V);
      if (eig_vecs[0]->Location() == QUDA_CUDA_FIELD_LOCATION) {
        for (int i = 0; i < Nvec; i++) {
          *eig_vecs[i] = *tmp[i];
          delete tmp[i];
        }
      }

      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Done loading vectors\n");
    } else {
      errorQuda("No eigenspace input file defined.");
    }
#else
    errorQuda("\nQIO library was not built.\n");
#endif
    // profile.TPSTOP(QUDA_PROFILE_IO);
    // profile.TPSTART(QUDA_PROFILE_COMPUTE);

    return;
  }

  void EigenSolver::saveVectors(const std::vector<ColorSpinorField *> &eig_vecs, std::string vec_outfile)
  {
    // profile.TPSTOP(QUDA_PROFILE_COMPUTE);
    // profile.TPSTART(QUDA_PROFILE_IO);

#ifdef HAVE_QIO
    const int Nvec = eig_vecs.size();
    std::vector<ColorSpinorField *> tmp;
    if (eig_vecs[0]->Location() == QUDA_CUDA_FIELD_LOCATION) {
      ColorSpinorParam csParam(*eig_vecs[0]);
      csParam.fieldOrder = QUDA_SPACE_SPIN_COLOR_FIELD_ORDER;
      csParam.setPrecision(eig_vecs[0]->Precision() < QUDA_SINGLE_PRECISION ? QUDA_SINGLE_PRECISION :
                                                                              eig_vecs[0]->Precision());
      csParam.location = QUDA_CPU_FIELD_LOCATION;
      csParam.create = QUDA_NULL_FIELD_CREATE;
      for (int i = 0; i < Nvec; i++) {
        tmp.push_back(ColorSpinorField::Create(csParam));
        *tmp[i] = *eig_vecs[i];
      }
    } else {
      for (int i = 0; i < Nvec; i++) { tmp.push_back(eig_vecs[i]); }
    }

    if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Start saving %d vectors to %s\n", Nvec, vec_outfile.c_str());

    void **V = static_cast<void **>(safe_malloc(Nvec * sizeof(void *)));
    for (int i = 0; i < Nvec; i++) {
      V[i] = tmp[i]->V();
      if (V[i] == NULL) {
        if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Could not allocate space for eigenVector[%04d]\n", i);
      }
    }

    write_spinor_field(vec_outfile.c_str(), &V[0], tmp[0]->Precision(), tmp[0]->X(),
                       tmp[0]->Ncolor(), tmp[0]->Nspin(), Nvec, 0, (char **)0);

    host_free(V);
    if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Done saving vectors\n");
    if (eig_vecs[0]->Location() == QUDA_CUDA_FIELD_LOCATION) {
      for (int i = 0; i < Nvec; i++) delete tmp[i];
    }

#else
    errorQuda("\nQIO library was not built.\n");
#endif
    // profile.TPSTOP(QUDA_PROFILE_IO);
    // profile.TPSTART(QUDA_PROFILE_COMPUTE);

    return;
  }

  void EigenSolver::loadFromFile(const DiracMatrix &mat, std::vector<ColorSpinorField *> &kSpace,
                                 std::vector<Complex> &evals)
  {

    if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Loading eigenvectors\n");
    loadVectors(kSpace, eig_param->vec_infile);

    // Create the device side residual vector by cloning
    // the kSpace passed to the function.
    ColorSpinorParam csParam(*kSpace[0]);
    csParam.create = QUDA_ZERO_FIELD_CREATE;
    r.push_back(ColorSpinorField::Create(csParam));

    // Error estimates (residua) given by ||A*vec - lambda*vec||
    computeEvals(mat, kSpace, evals, nEv);
    for (int i = 0; i < nEv; i++) {
      if (getVerbosity() >= QUDA_SUMMARIZE)
        printfQuda("EigValue[%04d]: (%+.16e, %+.16e) residual %.16e\n", i, evals[i].real(), evals[i].imag(), residua[i]);
    }

    delete r[0];
    return;
  }

  EigenSolver::~EigenSolver() {}
  //-----------------------------------------------------------------------------
  //-----------------------------------------------------------------------------

  // Thick Restarted Lanczos Method constructor
  TRLM::TRLM(QudaEigParam *eig_param, const DiracMatrix &mat, TimeProfile &profile) :
    EigenSolver(eig_param, profile),
    mat(mat)
  {

    // Tridiagonal/Arrow matrix
    alpha = new double[nKr];
    beta = new double[nKr];
    for (int i = 0; i < nKr; i++) {
      alpha[i] = 0.0;
      beta[i] = 0.0;
    }

    // Thick restart specific checks
    if (nKr < nEv + 6) errorQuda("nKr=%d must be greater than nEv+6=%d\n", nKr, nEv + 6);

    if (eig_param->eig_type == QUDA_EIG_LANCZOS
        && !(eig_param->spectrum == QUDA_SPECTRUM_LR_EIG || eig_param->spectrum == QUDA_SPECTRUM_SR_EIG)) {
      errorQuda("Only real spectrum type (LR or SR) can be passed to the Lanczos solver");
    }
  }

  void TRLM::operator()(std::vector<ColorSpinorField *> &kSpace, std::vector<Complex> &evals)
  {

    // Check to see if we are loading eigenvectors
    if (strcmp(eig_param->vec_infile, "") != 0) {
      loadFromFile(mat, kSpace, evals);
      return;
    }

    // Test for an initial guess
    double norm = sqrt(blas::norm2(*kSpace[0]));
    if (norm == 0) {

      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Initial residual is zero. Populating with rands.\n");
      if (kSpace[0]->Location() == QUDA_CPU_FIELD_LOCATION) {
        kSpace[0]->Source(QUDA_RANDOM_SOURCE);
      } else {
        RNG *rng = new RNG(kSpace[0]->Volume(), 1234, kSpace[0]->X());
        rng->Init();
        spinorNoise(*kSpace[0], *rng, QUDA_NOISE_UNIFORM);
        rng->Release();
        delete rng;
      }
    }

    // Normalise initial guess
    norm = sqrt(blas::norm2(*kSpace[0]));
    blas::ax(1.0 / norm, *kSpace[0]);

    // Create a device side residual vector by cloning
    // the kSpace passed to the function.
    ColorSpinorParam csParamClone(*kSpace[0]);
    csParam = csParamClone;
    // Increase Krylov space by one vector
    kSpace.push_back(ColorSpinorField::Create(csParam));
    csParam.create = QUDA_ZERO_FIELD_CREATE;
    r.push_back(ColorSpinorField::Create(csParam));
    //---------------------------------------------------------------------------

    // Convergence and locking criteria
    double mat_norm = 0.0;
    double epsilon = DBL_EPSILON;
    QudaPrecision prec = kSpace[0]->Precision();
    if (prec == QUDA_DOUBLE_PRECISION) {
      epsilon = DBL_EPSILON;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in double precision\n");
    }
    if (prec == QUDA_SINGLE_PRECISION) {
      epsilon = FLT_EPSILON;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in single precision\n");
    }
    if (prec == QUDA_HALF_PRECISION) {
      epsilon = 2e-3;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in half precision\n");
    }
    if (prec == QUDA_QUARTER_PRECISION) {
      epsilon = 5e-2;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in quarter precision\n");
    }

    // Begin TRLM Eigensolver computation
    //---------------------------------------------------------------------------
    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("*****************************\n");
      printfQuda("**** START TRLM SOLUTION ****\n");
      printfQuda("*****************************\n");
    }

    // Initial nEv step factorisation
    for (int step = 0; step < nEv; step++) lanczosStep(kSpace, step);
    iter += nEv;
    if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Initial %d step factorisation complete\n", nEv);

    // Loop over restart iterations.
    while (restart_iter < max_restarts && !converged) {

      for (int step = num_keep; step < nKr; step++) lanczosStep(kSpace, step);
      iter += (nKr - num_keep);
      // if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Restart %d complete\n", restart_iter+1);

      int arrow_pos = std::max(num_keep - num_locked + 1, 2);
      // The eigenvalues are returned in the alpha array and the
      eigensolveFromArrowMat(num_locked, arrow_pos);

      // mat_norm is updated.
      for (int i = num_locked; i < nKr; i++)
        if (fabs(alpha[i]) > mat_norm) mat_norm = fabs(alpha[i]);

      // Locking check
      iter_locked = 0;
      for (int i = 1; i < (nKr - num_locked); i++) {
        if (residua[i + num_locked] < epsilon * mat_norm) {
          if (getVerbosity() >= QUDA_DEBUG_VERBOSE)
            printfQuda("**** Locking %d resid=%+.6e condition=%.6e ****\n", i, residua[i + num_locked],
                       epsilon * mat_norm);
          iter_locked = i;
        } else {
          // Unlikely to find new locked pairs
          break;
        }
      }

      // Convergence check
      iter_converged = iter_locked;
      for (int i = iter_locked + 1; i < nKr - num_locked; i++) {
        if (residua[i + num_locked] < tol * mat_norm) {
          if (getVerbosity() >= QUDA_DEBUG_VERBOSE)
            printfQuda("**** Converged %d resid=%+.6e condition=%.6e ****\n", i, residua[i + num_locked], tol * mat_norm);
          iter_converged = i;
        } else {
          // Unlikely to find new converged pairs
          break;
        }
      }

      iter_keep = std::min(iter_converged + (nKr - num_converged) / 2, nKr - num_locked - 12);

      computeKeptRitz(kSpace);

      num_converged = num_locked + iter_converged;
      num_keep = num_locked + iter_keep;
      num_locked += iter_locked;

      if (getVerbosity() >= QUDA_SUMMARIZE) {
        // printfQuda("iter Conv = %d\n", iter_converged);
        // printfQuda("iter Keep = %d\n", iter_keep);
        // printfQuda("iter Lock = %d\n", iter_locked);
        printfQuda("%04d converged eigenvalues at restart iter %04d\n", num_converged, restart_iter + 1);
        // printfQuda("num_converged = %d\n", num_converged);
        // printfQuda("num_keep = %d\n", num_keep);
        // printfQuda("num_locked = %d\n", num_locked);
      }

      if (getVerbosity() >= QUDA_VERBOSE) {
        for (int i = 0; i < nKr; i++) {
          // printfQuda("Ritz[%d] = %.16e residual[%d] = %.16e\n", i, alpha[i], i, residua[i]);
        }
      }

      // Check for convergence
      if (num_converged >= nConv) {
        reorder(kSpace);
        converged = true;
      }

      restart_iter++;
    }

    if (getVerbosity() >= QUDA_DEBUG_VERBOSE)
      printfQuda("kSpace size at convergence/max restarts = %d\n", (int)kSpace.size());
    // Prune the Krylov space back to size when passed to eigensolver
    for (unsigned int i = nKr; i < kSpace.size(); i++) { delete kSpace[i]; }
    kSpace.resize(nKr);

    // Post computation report
    //---------------------------------------------------------------------------
    if (!converged) {
      if (getVerbosity() >= QUDA_SUMMARIZE) {
        printfQuda("TRLM failed to compute the requested %d vectors with a %d search space and %d Krylov space in %d "
                   "restart steps.\n",
                   nConv, nEv, nKr, max_restarts);
      }
    } else {
      if (getVerbosity() >= QUDA_SUMMARIZE) {
        printfQuda("TRLM computed the requested %d vectors in %d restart steps and %d OP*x operations.\n", nConv,
                   restart_iter, iter);

        // Dump all Ritz values and residua
        for (int i = 0; i < nEv; i++) {
          printfQuda("RitzValue[%04d]: (%+.16e, %+.16e) residual %.16e\n", i, alpha[i], 0.0, residua[i]);
        }
      }

      // Compute eigenvalues
      computeEvals(mat, kSpace, evals, nEv);
      if (getVerbosity() >= QUDA_SUMMARIZE) {
        for (int i = 0; i < nEv; i++) {
          printfQuda("EigValue[%04d]: (%+.16e, %+.16e) residual %.16e\n", i, evals[i].real(), evals[i].imag(),
                     residua[i]);
        }
      }

      // Compute SVD if requested
      time_svd = -clock();
      if (eig_param->compute_svd) { computeSVD(kSpace, evals); }
      time_svd += clock();
    }

    double total;

    if (eig_param->compute_svd)
      total = (time_e + time_mv + time_mb + time_svd) / CLOCKS_PER_SEC;
    else
      total = (time_e + time_mv + time_mb) / CLOCKS_PER_SEC;

    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("Time to solve problem using TRLM = %e\n", total);
      printfQuda("Time spent using EIGEN           = %e  %.1f%%\n", time_e / CLOCKS_PER_SEC,
                 100 * (time_e / CLOCKS_PER_SEC) / total);
      printfQuda("Time spent in matVec             = %e  %.1f%%\n", time_mv / CLOCKS_PER_SEC,
                 100 * (time_mv / CLOCKS_PER_SEC) / total);
      printfQuda("Time spent in (multi)blas        = %e  %.1f%%\n", time_mb / CLOCKS_PER_SEC,
                 100 * (time_mb / CLOCKS_PER_SEC) / total);
      if (eig_param->compute_svd)
        printfQuda("Time spent computing svd         = %e  %.1f%%\n", time_svd / CLOCKS_PER_SEC,
                   100 * (time_svd / CLOCKS_PER_SEC) / total);
    }
    //---------------------------------------------------------------------------

    // Local clean-up
    delete r[0];

    // Only save if outfile is defined
    if (strcmp(eig_param->vec_outfile, "") != 0) {
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("saving eigenvectors\n");
      saveVectors(kSpace, eig_param->vec_outfile);
    }

    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("*****************************\n");
      printfQuda("***** END TRLM SOLUTION *****\n");
      printfQuda("*****************************\n");
    }
  }

  // Destructor
  TRLM::~TRLM()
  {
    ritz_mat.clear();
    ritz_mat.shrink_to_fit();
    delete alpha;
    delete beta;
  }

  // Thick Restart Member functions
  //---------------------------------------------------------------------------
  void TRLM::lanczosStep(std::vector<ColorSpinorField *> v, int j)
  {

    // Compute r = A * v_j - b_{j-i} * v_{j-1}
    // r = A * v_j

    time_ = -clock();

    chebyOp(mat, *r[0], *v[j]);

    // a_j = v_j^dag * r
    alpha[j] = blas::reDotProduct(*v[j], *r[0]);

    // r = r - a_j * v_j
    blas::axpy(-alpha[j], *v[j], *r[0]);

    int start = (j > num_keep) ? j - 1 : 0;
    for (int i = start; i < j; i++) {

      // r = r - b_{j-1} * v_{j-1}
      blas::axpy(-beta[i], *v[i], *r[0]);
    }

    // Orthogonalise r against the Krylov space
    if (j > 0)
      for (int k = 0; k < 1; k++) blockOrthogonalize(v, r, j);

    // b_j = ||r||
    beta[j] = sqrt(blas::norm2(*r[0]));

    // Prepare next step.
    // v_{j+1} = r / b_j
    blas::zero(*v[j + 1]);
    blas::axpy(1.0 / beta[j], *r[0], *v[j + 1]);
  }

  void TRLM::reorder(std::vector<ColorSpinorField *> &kSpace)
  {

    int i = 0;

    if (reverse) {
      while (i < nKr) {
        if ((i == 0) || (alpha[i - 1] >= alpha[i]))
          i++;
        else {
          double tmp = alpha[i];
          alpha[i] = alpha[i - 1];
          alpha[--i] = tmp;
          *r[0] = *kSpace[i];
          *kSpace[i] = *kSpace[i - 1];
          *kSpace[i - 1] = *r[0];
        }
      }
    } else {
      while (i < nKr) {
        if ((i == 0) || (alpha[i - 1] <= alpha[i]))
          i++;
        else {
          double tmp = alpha[i];
          alpha[i] = alpha[i - 1];
          alpha[--i] = tmp;
          *r[0] = *kSpace[i];
          *kSpace[i] = *kSpace[i - 1];
          *kSpace[i - 1] = *r[0];
        }
      }
    }
  }

  void TRLM::eigensolveFromArrowMat(int num_locked, int arrow_pos)
  {

    int dim = nKr - num_locked;

    time_ = -clock();
    // Eigen objects
    MatrixXd A = MatrixXd::Zero(dim, dim);
    ritz_mat.resize(dim * dim);
    for (int i = 0; i < dim * dim; i++) ritz_mat[i] = 0.0;

    // Invert the spectrum due to chebyshev
    if (reverse) {
      for (int i = num_locked; i < nKr - 1; i++) {
        // printfQuda("Alpha[%d] = %e, beta[%d] = %e\n", i, alpha[i], i, beta[i]);
        alpha[i] *= -1.0;
        beta[i] *= -1.0;
      }
      alpha[nKr - 1] *= -1.0;
    }

    // Construct arrow mat A_{dim,dim}
    for (int i = 0; i < dim; i++) {

      // alpha populates the diagonal
      A(i, i) = alpha[i + num_locked];
    }

    for (int i = 0; i < arrow_pos - 1; i++) {

      // beta populates the arrow
      A(i, arrow_pos - 1) = beta[i + num_locked];
      A(arrow_pos - 1, i) = beta[i + num_locked];
    }

    for (int i = arrow_pos - 1; i < dim - 1; i++) {

      // beta populates the sub-diagonal
      A(i, i + 1) = beta[i + num_locked];
      A(i + 1, i) = beta[i + num_locked];
    }

    // Eigensolve the arrow matrix
    SelfAdjointEigenSolver<MatrixXd> eigensolver;
    eigensolver.compute(A);

    // repopulate ritz matrix
    for (int i = 0; i < dim; i++)
      for (int j = 0; j < dim; j++) ritz_mat[dim * i + j] = eigensolver.eigenvectors().col(i)[j];

    for (int i = 0; i < dim; i++) {
      residua[i + num_locked] = fabs(beta[nKr - 1] * eigensolver.eigenvectors().col(i)[dim - 1]);
      // Update the alpha array
      alpha[i + num_locked] = eigensolver.eigenvalues()[i];
    }

    // Put spectrum back in order
    if (reverse) {
      for (int i = num_locked; i < nKr; i++) { alpha[i] *= -1.0; }
    }

    time_ += clock();
    time_e += time_;
  }

  void TRLM::computeKeptRitz(std::vector<ColorSpinorField *> &kSpace)
  {

    int offset = nKr + 1;
    int dim = nKr - num_locked;

    if ((int)kSpace.size() < offset + iter_keep) {
      for (int i = kSpace.size(); i < offset + iter_keep; i++) {
        if (getVerbosity() >= QUDA_DEBUG_VERBOSE) printfQuda("Adding %d vector to kSpace\n", i);
        kSpace.push_back(ColorSpinorField::Create(csParam));
      }
    }

    for (int i = 0; i < iter_keep; i++) {
      int k = offset + i;
      *r[0] = *kSpace[num_locked];
      blas::ax(ritz_mat[dim * i], *r[0]);
      *kSpace[k] = *r[0];
      for (int j = 1; j < dim; j++) { blas::axpy(ritz_mat[i * dim + j], *kSpace[num_locked + j], *kSpace[k]); }
    }

    for (int i = 0; i < iter_keep; i++) *kSpace[i + num_locked] = *kSpace[offset + i];
    *kSpace[num_locked + iter_keep] = *kSpace[nKr];

    for (int i = 0; i < iter_keep; i++) beta[i + num_locked] = beta[nKr - 1] * ritz_mat[dim * (i + 1) - 1];
  }

  void TRLM::computeSVD(std::vector<ColorSpinorField *> &evecs, std::vector<Complex> &evals)
  {

    if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Computing SVD of M\n");

    int nConv = eig_param->nConv;
    Complex sigma_tmp[nConv / 2];

    for (int i = 0; i < nConv / 2; i++) {

      // This function assumes that you have computed the eigenvectors
      // of MdagM(MMdag), ie, the right(left) SVD of M. The ith eigen vector in the
      // array corresponds to the ith right(left) singular vector. We place the
      // computed left(right) singular vectors in the second half of the array. We
      // assume, in the comments, that right vectors are given and we compute the left.
      //
      // As a cross check, we recompute the singular values from mat vecs rather
      // than make the direct relation (sigma_i)^2 = |lambda_i|
      //--------------------------------------------------------------------------
      Complex lambda = evals[i];

      // M*Rev_i = M*Rsv_i = sigma_i Lsv_i
      mat.Expose()->M(*evecs[nConv / 2 + i], *evecs[i]);

      // sigma_i = sqrt(sigma_i (Lsv_i)^dag * sigma_i * Lsv_i )
      Complex sigma_sq = blas::cDotProduct(*evecs[nConv / 2 + i], *evecs[nConv / 2 + i]);
      sigma_tmp[i] = Complex(sqrt(sigma_sq.real()), sqrt(abs(sigma_sq.imag())));

      // Normalise the Lsv: sigma_i Lsv_i -> Lsv_i
      double norm = sqrt(blas::norm2(*evecs[nConv / 2 + i]));
      blas::ax(1.0 / norm, *evecs[nConv / 2 + i]);

      if (getVerbosity() >= QUDA_SUMMARIZE)
        printfQuda("Sval[%04d] = %+.16e  %+.16e   sigma - sqrt(|lambda|) = %+.16e\n", i, sigma_tmp[i].real(),
                   sigma_tmp[i].imag(), sigma_tmp[i].real() - sqrt(abs(lambda.real())));
      //--------------------------------------------------------------------------
    }

    // Update the host evals array
    for (int i = 0; i < nConv / 2; i++) {
      evals[2 * i + 0] = sigma_tmp[i];
      evals[2 * i + 1] = sigma_tmp[i];
    }
  }



  // --------------------------------------------------------------------------------------------------------------------------------

  // JD eigensolver class

  // Jacobi-Davidson Method constructor
  JD::JD(QudaEigParam *eig_param, const DiracMatrix &mat, TimeProfile &profile) :
    EigenSolver(eig_param, profile),
    mat(mat)
  { }

  void JD::operator()(std::vector<ColorSpinorField *> &eigSpace, std::vector<Complex> &evals)
  {
    // TODO: switch from k to iter

    int k=0, k_max, m=0, m_max, m_min;

    k_max = eig_param->nConv;
    max_restarts = eig_param->max_restarts;

    // TODO: extract these from command line params in a more general way
    m_max = eig_param->nKr;
    m_min = eig_param->nEv;

    // 'tau' is the <target> for the eigensolver
    double theta, mu, tau=0;

    // Check to see if we are loading eigenvectors
    if (strcmp(eig_param->vec_infile, "") != 0) {
      loadFromFile(mat, eigSpace, evals);
      return;
    }

    // Test for an initial guess
    double norm = sqrt(blas::norm2(*eigSpace[0]));
    if (norm == 0) {
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Initial residual is zero. Populating with rands.\n");
      if (eigSpace[0]->Location() == QUDA_CPU_FIELD_LOCATION) {
        eigSpace[0]->Source(QUDA_RANDOM_SOURCE);
      } else {
        RNG *rng = new RNG(eigSpace[0]->Volume(), 1234, eigSpace[0]->X());
        rng->Init();
        spinorNoise(*eigSpace[0], *rng, QUDA_NOISE_UNIFORM);
        rng->Release();
        delete rng;
      }
    }

    // Clone eigSpace's CSF params
    ColorSpinorParam csParam(*eigSpace[0]);

    // Init a zero residual
    csParam.create = QUDA_ZERO_FIELD_CREATE;
    r.push_back(ColorSpinorField::Create(csParam));

    double t1 = clock();

    // Convergence and locking criteria
    double epsilon = DBL_EPSILON;
    QudaPrecision prec = eigSpace[0]->Precision();
    if (prec == QUDA_DOUBLE_PRECISION) {
      epsilon = DBL_EPSILON;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in double precision\n");
    }
    if (prec == QUDA_SINGLE_PRECISION) {
      epsilon = FLT_EPSILON;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in single precision\n");
    }
    if (prec == QUDA_HALF_PRECISION) {
      epsilon = 2e-3;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in half precision\n");
    }
    if (prec == QUDA_QUARTER_PRECISION) {
      epsilon = 5e-2;
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Running Eigensolver in quarter precision\n");
    }

    // Begin JD Eigensolver computation
    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("*****************************\n");
      printfQuda("***** START JD SOLUTION *****\n");
      printfQuda("*****************************\n");
    }

    // Create a temporary vector t, which will be used for storing the result of the
    // inversion of the shifted-and-inverted version of MMdag
    std::vector<ColorSpinorField *> t;
    csParam.create = QUDA_COPY_FIELD_CREATE;
    // Copying initial guess
    t.push_back(ColorSpinorField::Create(*eigSpace[0], csParam));

    // Reusing eigSpace to store the output null vectors
    eigSpace.clear();

    // Create the vector subspaces used for faster searchs of eigenpairs
    std::vector<ColorSpinorField *> u, w, V, W, X_tilde;
    // Buffer spinors
    csParam.create = QUDA_ZERO_FIELD_CREATE;
    w.push_back(ColorSpinorField::Create(csParam));
    u.push_back(ColorSpinorField::Create(csParam));

    // Matrix with the compressed sub-space information to extract the eigenpairs
    MatrixXcd H;
    SelfAdjointEigenSolver<MatrixXcd> eigensolver;

    // TODO: change these -- is the second one still necessary ?
    eig_param->invert_param->verbosity = QUDA_SILENT;
    eig_param->invert_param->tol = 1e-1;

    // Main loop
    while (restart_iter<max_restarts && k<k_max) {

      // Compute: w = (D - tau*I)t, D = MMdag + shift
      matVec(mat, *w[0], *t[0]);
      if (tau != 0.0) blas::caxpy(-tau, const_cast<ColorSpinorField&>(*t[0]), *w[0]);

      // TODO: call this in a modularized way
      // Orthogonalization of w and t against W
      for(int i=0; i<m; i++){
        Complex gamma = blas::cDotProduct(*W[i], *w[0]);
        blas::caxpy(-gamma, *W[i], *w[0]);
        blas::caxpy(-gamma, *V[i], *t[0]);
      }

      m++;

      // TODO: call this in a modularized way
      // Normalisation of w and t, and push them into V and W
      norm = sqrt(blas::norm2(*w[0]));
      blas::ax(1.0 / norm, *w[0]);
      csParam.create = QUDA_COPY_FIELD_CREATE;
      W.push_back(ColorSpinorField::Create(*w[0], csParam));
      V.push_back(ColorSpinorField::Create(*t[0], csParam));
      blas::ax(1.0 / norm, *V[m-1]);

      // TODO: call this in a modularized way
      // Construction of H = WdagV
      H.conservativeResize(m,m);
      for(int i=0; i<m-1; i++){
        H(i,m-1) = blas::cDotProduct(*W[i], *V[m-1]);
        H(m-1,i) = conj(H(i,m-1));
      }
      H(m-1,m-1) = blas::cDotProduct(*W[m-1], *V[m-1]);

      // ith eigenvalue: eigensolver.eigenvalues()[i], ith eigenvector: eigensolver.eigenvectors().col(i)
      eigensolver.compute(H);

      // TODO: call this in a modularized way
      // Moving the eigenpairs to a vector of std::pair to sort by eigenvalue
      std::vector< std::pair < double, std::vector<Complex>* > > eigenpairs;
      std::vector<Complex>* buffVec = 0;
      for(int i=0; i<m; i++){
        // TODO: switch to safe_malloc ?
        buffVec = new std::vector<Complex>(eigensolver.eigenvectors().col(i).data(),
                                           eigensolver.eigenvectors().col(i).data() + eigensolver.eigenvectors().col(i).size());
        eigenpairs.push_back( std::make_pair( eigensolver.eigenvalues()[i], buffVec ) );
      }

      // Order the eigeninformation extracted from H in descending order of eigenvalues
      // Using sort+reverse avoids declaration of extra sort-function. Not so good for large subspaces ?
      std::sort(eigenpairs.begin(), eigenpairs.end());
      std::reverse(std::begin(eigenpairs), std::end(eigenpairs));

      // TODO: try to make the syntax more understandable
      // Computing the residual
      // u_tilde = V * s_1 -- lifting the first eigenvector through V
      blas::zero(*u[0]);
      for( int i=0; i<m; i++ ){
        blas::caxpy( (*(eigenpairs[0].second))[i], *V[i], *u[0] );
      }
      // mu = norm( u_tilde )
      norm = sqrt(blas::norm2(*u[0]));
      // u = normalized u_tilde
      blas::ax(1.0 / norm, *u[0]);
      // theta_tilde = first eigenvalue / mu^2
      theta = eigenpairs[0].first / (norm*norm);
      // w_tilde = W * s_1
      blas::zero(*r[0]);
      for( int i=0; i<m; i++ ){
        blas::caxpy( (*(eigenpairs[0].second))[i], *W[i], *r[0] );
      }
      // r = w_tilde / mu - theta * u
      blas::ax(1.0 / norm, *r[0]);
      blas::caxpy(-theta, *u[0], *r[0]);

      // Loop while || r || < tol
      norm = sqrt(blas::norm2(*r[0]));
      // TODO: wrap this printfQuda with verbosity
      printfQuda("\nNorm of the residual = %f\n", norm);
      while(norm < tol){

        printfQuda("\n\nOne of the residuals hit !\n\n\n");

        // TODO !

        //k++;

        //X.push_back(new ColorSpinorField(*u[0]));
        //evals.push_back(theta + tau);

        //if(k == k_max){
          //TODO: this has to be modularized... because this if statement has to return out of the whole JD
          /*
          if (getVerbosity() >= QUDA_DEBUG_VERBOSE)
            printfQuda("**** Converged %d resid=%+.6e condition=%.6e ****\n", i, residua[i + num_locked], tol * mat_norm);
          iter_converged = i;
          */
        //}

        //m--;

        //H = MatrixXcf::Zero(m,m);

        /*
        for(int i=0; i<m; i++){

          //blas::zero(*V[i]);
          blas::caxpy(eigensolver.eigenvectors().col(i+1)[i], *V[i], *V[i]);
          for( int j=0; j<m; j++ ){
            if(i==j){ continue; }
            blas::caxpy(eigensolver.eigenvectors().col(i+1)[j], *V[j], *V[i]);
          }
          //blas::zero(*W[i]);
          blas::caxpy(eigensolver.eigenvectors().col(i+1)[i], *W[i], *W[i]);
          for( int j=0; j<m; j++ ){
            if(i==j){ continue; }
            blas::caxpy(eigensolver.eigenvectors().col(i+1)[j], *W[j], *W[i]);
          }

          H(i,i) = eigensolver.eigenvalues()[i+1];

          //set the first column of eigenvectors to be a unit vector
          //TODO: change the following line for the appropriate one !
          eigensolver.eigenvectors().col(i)[0] = 0;
          for(int j=1; j<m; j++){
            //TODO: change the following line for the appropriate one !
            eigensolver.eigenvectors().col(i)[j] = 0;
          }
          //TODO: change the following line for the appropriate one !
          eigensolver.eigenvalues()[i+1] = eigensolver.eigenvalues()[i];

        }
        */

        //mu = sqrt(blas::norm2(*V[0]));

        //theta = eigensolver.eigenvalues()[1] / (mu*mu);

        //*u[0] = *V[0]);
        //blas::ax(1.0 / mu, *V[0]);

        //*r[0] = *W[0];
        //blas::ax(1.0 / mu, *r[0]);
        //blas::caxpy(-theta, *u[0], *r[0]);

        // TODO: remove the following hardcoded exit
        norm = 1.0;

      }

      // Restart: shrink the acceleration subspace
      if(m >= m_max){
        // TODO: change this section to a much more efficient way; mainly tmpV and tmpW ---> bring them to an outter scope

        csParam.create = QUDA_ZERO_FIELD_CREATE;
        std::vector<ColorSpinorField*> tmpV, tmpW;

        MatrixXcd tmpH = MatrixXcd::Zero(m_min,m_min);

        csParam.create = QUDA_ZERO_FIELD_CREATE;
        for(int i=0; i<m_min; i++){
          tmpV.push_back(ColorSpinorField::Create(csParam));
          tmpW.push_back(ColorSpinorField::Create(csParam));
          for( int j=0; j<m; j++ ){
            blas::caxpy( (*(eigenpairs[i].second))[j], *V[j], *tmpV[i] );
            blas::caxpy( (*(eigenpairs[i].second))[j], *W[j], *tmpW[i] );
            tmpH(i,i) = eigenpairs[i].first;
          }
        }

        m = m_min;

        // Assign new values of H
        // TODO: skip this resize() ?
        H.resize(m_min,m_min);
        H = tmpH;

        // Assign new values of V and W
        for (auto p : V){ delete p; }
        for (auto p : W){ delete p; }
        V = tmpV;
        W = tmpW;

        restart_iter++;
      }

      // Updating shift value
      theta = theta + tau;

      // Expansion of the projection space. The proj op is (I - QQdag), with Q eq to eigSpace
      eigSpace.push_back( u[0] );

      // TODO: change this. Make an appropriate use of profile
      if(profile.isRunning(QUDA_PROFILE_COMPUTE)){ profile.TPSTOP(QUDA_PROFILE_COMPUTE); }

      // TODO: simply remove these ?
      QudaVerbosity verbTmp = getVerbosity();
      setVerbosity(QUDA_SILENT);

      // TODO: change this section to a much more efficient way; alloc/dealloc - ating Dirac stuff (m's and d's) too inefficient
      // Proposing a new vector t through the solution of a shifted-and-projected MMdag
      {
        //The matrix solvers for the shifted-and-proj MMdag
        DiracMatrix *mm, *mmSloppy;

        double bareShift_mm, bareShift_mmSloppy;

        Dirac *d = nullptr;
        Dirac *dSloppy = nullptr;

        // TODO: call this in a modularized way
        // Create the dirac operator
        {
          bool pc_solve = (eig_param->invert_param->solve_type == QUDA_DIRECT_PC_SOLVE) || (eig_param->invert_param->solve_type == QUDA_NORMOP_PC_SOLVE);

          DiracParam diracParam;
          DiracParam diracSloppyParam;

          quda::setDiracParam(diracParam, eig_param->invert_param, pc_solve);
          quda::setDiracSloppyParam(diracSloppyParam, eig_param->invert_param, pc_solve);

          d = Dirac::create(diracParam); // create the Dirac operator
          dSloppy = Dirac::create(diracSloppyParam);
        }

        Dirac &dirac = *d;
        Dirac &diracSloppy = *dSloppy;

        mm = new DiracProjMMdagProj(dirac);
        mm->projSpace = eigSpace;

        mmSloppy = new DiracProjMMdagProj(diracSloppy);
        mmSloppy->projSpace = eigSpace;

        // Switching to the appropriate shift for JD
        bareShift_mm = mm->shift;
        mm->shift = bareShift_mm - theta;
        bareShift_mmSloppy = mmSloppy->shift;
        mmSloppy->shift = bareShift_mmSloppy - theta;

        QudaInvertParam refineparam = *eig_param->invert_param;
        refineparam.cuda_prec_sloppy = eig_param->invert_param->cuda_prec_refinement_sloppy;

	SolverParam solverParam(refineparam);
	solverParam.iter = 0;
	solverParam.use_init_guess = QUDA_USE_INIT_GUESS_YES;
	//solverParam.tol = (param->tol_offset[i] > 0.0 ?  param->tol_offset[i] : iter_tol); // set L2 tolerance
	solverParam.tol = 1e0;
	//solverParam.tol_hq = param->tol_hq_offset[i]; // set heavy quark tolerance
        solverParam.delta = eig_param->invert_param->reliable_delta_refinement;

        {
          CG cg(*mmSloppy, *mmSloppy, solverParam, profile);
          cg(*t[0], *r[0]);
        }

        // Switching back the shift parameters
        mm->shift = bareShift_mm;
        mmSloppy->shift = bareShift_mmSloppy;

        // Clearing allocated data
        delete mm;
        delete mmSloppy;
        delete d;
        delete dSloppy;

        // TODO: call ?
        // cache is written out even if a long benchmarking job gets interrupted
        //saveTuneCache();
      }

      // TODO: simply remove this ?
      setVerbosity(verbTmp);

      eigSpace.pop_back();

      //computeKeptRitz(kSpace); //TODO: will this be of any use??

      if (getVerbosity() >= QUDA_SUMMARIZE) {
        // printfQuda("iter Conv = %d\n", iter_converged);
        // printfQuda("iter Keep = %d\n", iter_keep);
        // printfQuda("iter Lock = %d\n", iter_locked);
        printfQuda("%04d converged eigenvalues at JD iter %04d\n", num_converged, m + 1);
        // printfQuda("num_converged = %d\n", num_converged);
        // printfQuda("num_keep = %d\n", num_keep);
        // printfQuda("num_locked = %d\n", num_locked);
      }

      if (getVerbosity() >= QUDA_VERBOSE) {
        //for (int i = 0; i < nKr; i++) {
          // printfQuda("Ritz[%d] = %.16e residual[%d] = %.16e\n", i, alpha[i], i, residua[i]);
        //}
      }

      // Clearing allocated memory for eigenpairs
      for (auto p : eigenpairs){ delete p.second; }
      eigenpairs.clear();

      //reorder(kSpace); //TODO: is this line useful somehow ??
    }

    //TODO: is this pruning step necessary for JD ?
    /*
    if (getVerbosity() >= QUDA_DEBUG_VERBOSE)
      printfQuda("kSpace size at convergence/max restarts = %d\n", kSpace.size());
    // Prune the Krylov space back to size when passed to eigensolver
    for (int i = nKr; i < kSpace.size(); i++) { delete kSpace[i]; }
    kSpace.resize(nKr);
    */

    // Post computation report

    if (!converged) {
      if (getVerbosity() >= QUDA_SUMMARIZE) {
        //printfQuda("TRLM failed to compute the requested %d vectors with a %d search space and %d Krylov space in %d "
        //           "restart steps.\n",
        //           nConv, nEv, nKr, max_restarts);
        printfQuda("JD failed to compute the requested eigenpairs.\n");
      }
    } else {
      if (getVerbosity() >= QUDA_SUMMARIZE) {
        printfQuda("JD computed the requested %d vectors in %d restart steps and %d OP*x operations.\n", nConv,
                   restart_iter, iter); //TODO: very important ---> get the values of iter counted properly in JD

        // Dump all Ritz values and residua
        //for (int i = 0; i < nEv; i++) {
          //printfQuda("RitzValue[%04d]: (%+.16e, %+.16e) residual %.16e\n", i, alpha[i], 0.0, residua[i]);
          //TODO: how to print this in the case of JD ?? Is it really necessary to display/analyze ??
        //}
      }

      // Compute eigenvalues //TODO: double-check that computeEvals(...) is general/applicable for JD
      //computeEvals(mat, kSpace, evals, nEv);
      //if (getVerbosity() >= QUDA_SUMMARIZE) {
      //  for (int i = 0; i < nEv; i++) {
      //    printfQuda("EigValue[%04d]: (%+.16e, %+.16e) residual %.16e\n", i, evals[i].real(), evals[i].imag(),
      //               residua[i]);
      //  }
      //}

      // Compute SVD if requested -- TODO: is this also usable in JD ??
      /*
      time_svd = -clock();
      if (eig_param->compute_svd) computeSVD(kSpace, d_vecs_tmp, evals, reverse);
      time_svd += clock();
      */
    }

    double t2 = clock() - t1;
    double total;

    if (eig_param->compute_svd)
      total = (time_e + time_mv + time_mb + time_svd) / CLOCKS_PER_SEC;
    else
      total = (time_e + time_mv + time_mb) / CLOCKS_PER_SEC;

    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("Time to solve problem using JD = %e\n", t2 / CLOCKS_PER_SEC);
      printfQuda("Time spent using EIGEN           = %e  %.1f%%\n", time_e / CLOCKS_PER_SEC,
                 100 * (time_e / CLOCKS_PER_SEC) / total);
      printfQuda("Time spent in matVec             = %e  %.1f%%\n", time_mv / CLOCKS_PER_SEC,
                 100 * (time_mv / CLOCKS_PER_SEC) / total);
      printfQuda("Time spent in (multi)blas        = %e  %.1f%%\n", time_mb / CLOCKS_PER_SEC,
                 100 * (time_mb / CLOCKS_PER_SEC) / total);
      if (eig_param->compute_svd)
        printfQuda("Time spent computing svd         = %e  %.1f%%\n", time_svd / CLOCKS_PER_SEC,
                   100 * (time_svd / CLOCKS_PER_SEC) / total);
    }

    // Local clean-up
    delete r[0];
    delete t[0];
    delete w[0];
    delete u[0];
    for (auto p : V){ delete p; }
    for (auto p : W){ delete p; }

    // Only save if outfile is defined -- exactly as in TRLM
    if (strcmp(eig_param->vec_outfile, "") != 0) {
      if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("saving eigenvectors\n");
      if(eigSpace.size()>0) { saveVectors(eigSpace, eig_param->vec_outfile); }
    }

    csParam.create = QUDA_ZERO_FIELD_CREATE;
    if(eigSpace.size() == 0) { eigSpace.push_back(ColorSpinorField::Create(csParam)); }

    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("*****************************\n");
      printfQuda("****** END JD SOLUTION ******\n");
      printfQuda("*****************************\n");
    }
  }

  // Destructor
  JD::~JD()
  {
    //ritz_mat.clear();
    //ritz_mat.shrink_to_fit();
    //delete alpha;
    //delete beta;
  }


} // namespace quda
