# WaveWorks CPU FFT and spectral evolution: from theory to implementation

This page is organized as a derivation followed by an implementation contract. We begin with the
periodic Fourier representation and linear water-wave equations, derive the Local and Wind
algorithms, and only then describe the recovered CPU data layout, numerical approximations, task
graph, and verification rules. This order answers both questions: **why** each operation is valid
and **how** this repository performs it.

The scope is the numerical CPU implementation of Local Waves and Wind Waves; rendering-only
consumers are outside this page. Recovered behavior takes precedence when reproducing this SDK,
even when it differs from a modern or textbook normalization.

Statements are marked as **mathematical definitions**, **reference models**, or **recovered
behavior**. Source tags such as `[T01]` and `[WWS-CPU]` are defined in the final references
chapter.

Reading path: periodic representation → efficient FFT → wave and spectrum theory → derived
algorithms → CPU contracts and solver implementations → task pipeline → verification.

| Part | Chapters | Purpose |
|---|---:|---|
| Preface | 0 | Notation, units, and transform conventions used throughout the page |
| I — Mathematical foundations | 1–2 | Periodic Fourier representation and efficient radix-2 evaluation |
| II — Physical/statistical models | 3–4 | Linear water waves, `S(k)`, and stochastic spectrum construction |
| III — Derived algorithms | 5–6 | Modal evolution, displacement theory, and end-to-end Local/Wind algorithms |
| IV — CPU implementation | 7 | Recovered numerical contracts and source-compatible solver behavior |
| V — Verification/provenance | 8–11 | Task pipeline, validation rules, equation map, and references |

## 0. Notation and conventions

| Symbol | Meaning in this page |
|---|---|
| `N` | Samples per axis; the recovered radix-2 path requires a supported power-of-two size. |
| `L` (or `Lx,Ly`) | Physical periodic domain length in metres. |
| `η(x,y,t)` | Real spatial height/displacement field. |
| `H(kx,ky,t)` or `h̃(k,t)` | Complex Fourier coefficient; `H` is the unnormalized grid DFT, while `h̃` denotes a paper's Fourier-series coefficient. |
| `q=(qx,qy)` | Dimensionless centered integer wave index; `k=(2π/L)q` is the physical wave vector in radians per metre. |
| `ω(k)` | Angular frequency in radians per second; `c_g=dω/d|k|` is radial group velocity. |
| `i` | Imaginary unit, `i²=-1`; it is not a grid index when used in an exponential. |
| `t` and `Δt_kick` | Wind's absolute time and Local's per-kick delta time, respectively; source member names are not interchangeable. |

## Part I — Mathematical foundations

### 1. Periodic spectral representation

#### 1.1 Periodic Fourier series and the physical wave-vector grid

For a rectangular periodic domain with side lengths `Lx` and `Ly`, the admissible wave vectors
are discrete because a Fourier mode must satisfy `exp(i kx Lx)=exp(i ky Ly)=1`:

```text
kx = 2π nx/Lx,       ky = 2π ny/Ly,       nx,ny ∈ ℤ.
```

Writing `k=(kx,ky)`, a real surface is represented by the Fourier series

```text
η(x,y,t) = Σ_k h̃(k,t) exp(i k·(x,y)).
```

This is the starting point of Tessendorf's ocean formulation (his Eq.19). For the square `N×N`
grid used by this repository, `Δx=L/N` and `Δk=2π/L`; a rectangular grid would instead use
`Δkx=2π/Lx` and `Δky=2π/Ly`. The finite sum uses integer indices modulo `N`. The continuum and
discrete coefficients differ by the chosen `1/N²`, `Δk²`, and `(2π)²` factors, which is why a
paper's `P_h(k)` should not be copied into this SDK without checking its normalization.

The finite-grid transform and its normalization are defined in §1.2. Keeping those factors
explicit is important: a paper's Fourier-series coefficient, a unitary DFT coefficient, and the
recovered CPU buffer are different numerical quantities. Tessendorf's Fourier expansion and
wave-number grid are in [T01, Eq.19](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf).

#### 1.2 DFT and inverse-DFT definitions

For row-major samples `η[x,y]`, define the normalized mathematical operators explicitly:

```text
FFTrows(η)[kx,y] = Σ(x=0..N-1) η[x,y] exp(-i 2π kx x/N)
FFTcolumns(R)[kx,ky] = Σ(y=0..N-1) R[kx,y] exp(-i 2π ky y/N)

IFFTrows(R)[x,y] = (1/N) Σ(kx=0..N-1) R[kx,y] exp(+i 2π kx x/N)
IFFTcolumns(F)[x,y] = (1/N) Σ(ky=0..N-1) F[x,ky] exp(+i 2π ky y/N)

FFT2D(η) = FFTcolumns(FFTrows(η))
IFFT2D(H) = IFFTrows(IFFTcolumns(H))

IFFT2D(H)[x,y] = (1/N²) Σ(ky=0..N-1) Σ(kx=0..N-1)
                 H[kx,ky] exp(+i 2π(kx x + ky y)/N)
```

The character orthogonality relation
`Σ_x exp(i2π(p-r)x/N)=N·1[p=r]` makes the inverse recover one sample and cancel all other
modes. For the unnormalized forward transform this gives Parseval's identity:

```text
Σ_{x,y}|η[x,y]|² = (1/N²) Σ_{kx,ky}|H[kx,ky]|².
```

`FFTcolumns` transforms a fixed-`kx` column over `y`; it does not imply a matrix transpose.
The two axis operators commute mathematically, although their memory strides and CPU scheduling
order differ. The equations here use a normalized inverse; the recovered unnormalized CPU ABI is
defined in Chapter 7. [FFTW](https://www.fftw.org/fftw3_doc/Multi_002ddimensional-Transforms.html)

#### 1.3 Two-dimensional separability and row/column operators

The 2-D kernel factors into independent characters:

```text
exp[-i 2π(pm+qn)/N]
  = exp[-i 2πpm/N] · exp[-i 2πqn/N].
```

Define an intermediate row transform

```text
R[p,n] = Σ_m η[m,n] exp(-i 2πpm/N).
```

Then

```text
H[p,q] = Σ_n R[p,n] exp(-i 2πqn/N).
```

The first sum is `FFTrows`; the second is `FFTcolumns`. If the matrix is stored as
`A[m,n]=η[m,n]` (row index `m`, column index `n`), then the corresponding matrix notation is
`H = F_N A F_Nᵀ`, with `H[p,q]` as defined above; in vector notation,
`vec(H)=(F_N⊗F_N)vec(A)` (up to the chosen vectorization order). The tensor-product
view explains both the mathematical separability and the implementation's two independent line
passes. There are `N` length-`N` transforms in each direction, so the total work is
`2N·Θ(N log₂N)=Θ(N² log₂N)`. A direct four-index DFT would be `Θ(N⁴)`.

Separability does **not** require transposing the matrix. A transpose can make the second pass
contiguous, but it adds another full-grid memory movement. This implementation keeps the original
row-major layout and gathers four lines into a temporary lane buffer. The trade-off is visible in
the source: `FFT1DSIMD_X_4wide` packs four rows, while `FFT1DSIMD_Y_4wide` packs four adjacent
columns and then scatters them back. [WWS-CPU](sdk/src/Simulation_Util.cpp#L281-L356)

#### 1.4 Centered indices, Hermitian symmetry, Nyquist modes, and aliasing

The centered integer index for an even grid is commonly written

```text
q(i) = i,       0 ≤ i < N/2,
       i-N,     N/2 ≤ i < N.
```

The negative partner is `(-q) mod N`, not necessarily the visual mirror `N-1-i`. In particular,
the Nyquist index `N/2` is its own negative modulo `N`. A real field therefore has four
self-conjugate 2-D bins (`(0,0)`, `(N/2,0)`, `(0,N/2)`, `(N/2,N/2)`), whose imaginary parts must
be zero. All other bins occur in conjugate pairs.

The recovered Local mirror and Wind padded-endpoint conventions are implementation contracts in Chapter 7; the mathematical partner remains `(-q) mod N`.

Sampling cannot represent frequencies above Nyquist. A linear FFT pass preserves the represented
modes, but multiplying fields or forming a steep choppy displacement creates sums of wave vectors
(`k₁+k₂`) that can exceed Nyquist and alias back into the grid. The recovered code does not
implement a general 2/3-rule de-aliasing filter; radial cascade windows, small-wave cutoffs,
finite `N`, and the choppy scale are the practical controls. Tests should compare the spectral
planes before any nonlinear consumer of the displacement.

For a first derivative, a strict spectral implementation sets the multiplier at the one-dimensional
Nyquist index `q=N/2` to zero: that mode is self-conjugate and has no unique odd (derivative) sign.
The recovered CPU displacement path avoids a derivative at `k=0`, but does not expose a separate
Nyquist derivative filter. A port that computes gradients as `i k H` should make this choice
explicit, following [Johnson's FFT differentiation note](https://math.mit.edu/~stevenj/fft-deriv.pdf),
or document why its checkerboard/axis convention intentionally differs.

#### 1.5 Finite-torus interpretation

The grid is the finite abelian group `G = Z_N × Z_N`. Its Fourier basis functions (characters)

```text
χ_q(x) = exp(-i 2π (qx x + qy y)/N)
```

obey `χ_q(x+x')=χ_q(x)χ_q(x')` and the orthogonality relation
`Σ_x χ_q(x) conjugate(χ_r(x)) = N² δ[q-r]`. The DFT is therefore a change of coordinates from
point samples to an orthogonal character basis, not merely a fast summation trick. A periodic
translation `T_a f(x)=f(x-a)` acts on one character by
`T_aχ_q = χ_q(-a)χ_q = conjugate(χ_q(a))χ_q`; every translation-invariant convolution is consequently diagonal in
Fourier space. This is why the same FFT can evolve every wave number independently and why a
constant-coefficient spatial operator can be represented by one scalar multiplier per bin.

For comparison, a finite-difference Laplacian would have symbol

```text
λ_Δ(q) = -4 [ sin²(π qx/N)/Δx² + sin²(π qy/N)/Δy² ],
```

whereas the continuum deep-water Dirichlet--Neumann operator has symbol `|k|`. WaveWorks uses
the continuum `k=(2π/L)q` in its dispersion while using the discrete FFT for transforms; replacing
`|k|` with the finite-difference symbol changes the physical model, not just the numerical
implementation. This distinction is useful when comparing an FFT ocean solver with a stencil
solver. [T01](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf)
and [FFTW's multidimensional DFT definition](https://www.fftw.org/fftw3_doc/Multi_002ddimensional-Transforms.html)

### 2. Radix-2 FFT algorithm

#### 2.1 Cooley–Tukey factorization and the radix-2 butterfly

Let `W_N=exp(-i2π/N)` and split the input index as either `2r` or `2r+1`:

```text
E[k] = Σ_{r=0}^{N/2-1} x[2r]   W_{N/2}^{rk}
O[k] = Σ_{r=0}^{N/2-1} x[2r+1] W_{N/2}^{rk}.
```

For `0≤k<N/2`,

```text
X[k]       = E[k] + W_N^k O[k]
X[k+N/2]   = E[k] - W_N^k O[k].
```

The second output reuses the first twiddle because
`W_N^{k+N/2}=W_N^k·exp(-iπ)=-W_N^k`. This is the radix-2 butterfly. Recursing gives
`T(N)=2T(N/2)+N`, hence `T(N)=N log₂N` butterflies up to constant factors. Cooley and Tukey
introduced this factorization for machine calculation in [CT65](https://doi.org/10.1090/S0025-5718-1965-0178586-1).

The recursive dependency graph is reordered into an iterative in-place loop by reversing the
`log₂N` bits of each index before stage one. For example, with `N=8`, `001₂` is stored at
`100₂`; the source swaps only when `i<reverse(i)` so each pair moves once. At stage `s`:

```text
span     = 2^(s+1)
halfSpan= span/2
for each block and j in [0,halfSpan):
    t     = W_span^j · old_b
    new_a = old_a + t
    new_b = old_a - t
    data[block+j]          = new_a
    data[block+j+halfSpan] = new_b
```

The implementation keeps copies of both old butterfly inputs so the last expression is evaluated
as `old_a-t`, not from an already overwritten value. Its `wm` starts at the length-two root,
`w` advances by complex multiplication for each `offset`, and a half-angle square-root computes
the root for the next stage. This moves trigonometric work out of the inner butterfly but lets
round-off in `w←w·wm` accumulate. [WWS-CPU](sdk/src/Simulation_Util.cpp#L198-L279)

#### 2.2 Complexity and mathematical preconditions

The radix-2 recurrence has `Θ(N log₂N)` work for one line and `Θ(N² log₂N)` work for an `N×N`
transform. The mathematical algorithm requires a power-of-two line length. The fixed CPU task
partition adds a separate operational precondition: public sizes must also be large enough and
divisible by the line-block widths used by the Local and Wind schedulers. The exact checks and
failure mode are recorded in Chapter 7.

## Part II — Physical and statistical wave models

### 3. Linear water-wave theory

#### 3.1 Potential flow and one oscillator per Fourier mode

The Local recurrence is motivated by the exact solution of the
linearized free-surface equations behind Tessendorf's interactive-wave model ([T04](https://jtessen.people.clemson.edu/reports/papers_files/Interactive_Water_Surfaces.pdf));
the explicit matrix/exponential propagator is documented in [EW14](https://jtessen.people.clemson.edu/reports/papers_files/ewavealgorithm.pdf):

1. Water is incompressible, `∇·u=0`.
2. Flow is irrotational, `∇×u=0`, so `u=∇φ` for a velocity potential `φ`.
3. Therefore `∇²φ=0` in the fluid.
4. Linearizing the kinematic and dynamic free-surface conditions at `z=0` gives, for gravity
   waves, `η_t=φ_z` and `φ_t+gη=0`.

For a deep-water Fourier mode, the decaying solution of Laplace's equation is
`φ_k(z,t)=φ_k(0,t)e^{|k|z}`. Thus `φ_z(0)=|k|φ_k(0)` and

```text
η̇_k = |k| φ_k,
φ̇_k = -g η_k,
η̈_k + g|k| η_k = 0.
```

The square-root operator notation in the eWave/iWave derivation is
`η_t = sqrt(-∇²) φ`: on the eigenfunction `e^{ik·x}`, `-∇²` has eigenvalue `|k|²`, so its
positive square root is multiplication by `|k|`. This is why the FFT diagonalizes the spatial
PDE and turns a coupled field into independent oscillators. T04 supplies the interactive-wave
PDE motivation; EW14 supplies the exact exponential update used in §3.3.

#### 3.2 Gravity, capillarity, finite depth, and dispersion

Surface tension adds the linearized curvature pressure:

```text
φ_t + gη - (σ/ρ) ∇²η = 0.
```

For one mode this changes the oscillator to

```text
η̈_k + [g|k| + (σ/ρ)|k|³] η_k = 0,
ω²(k) = g|k| + (σ/ρ)|k|³.              (deep water)
```

At finite depth `h`, the vertical eigenfunction changes and the complete Airy relation is

```text
ω²(k) = [g|k| + (σ/ρ)|k|³] tanh(|k|h).
```

The CPU source assumes deep water (`tanh→1`) and uses `σ/ρ≈0.074/1000=7.4e-5`; it does not
accept a depth parameter. The `0.000074·|k|³` term must therefore be documented as a
gravity--capillary correction, not as finite-depth damping or a depth approximation. See
[T01, Eqs.14--16](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf)
and the finite-depth gravity derivation in [MIT's free-surface-wave notes](https://ocw.mit.edu/courses/2-016-hydrodynamics-13-012-fall-2005/3133000c14ec08086951d724ff277b13_free_surf_wave.pdf).
The capillary finite-depth extension is given in [Hinch's notes](https://www.damtp.cam.ac.uk/user/hinch/teaching/D24c.pdf);
the latter is a theory comparison, not an additional parameter accepted by this CPU path.

It is useful to distinguish the raw surface potential from a normalized state channel. Let
`G_k = g+(σ/ρ)|k|²`, so that `ω²=|k|G_k`. In the deep-water linear equations the raw pair obeys

```text
d/dt [ η_k ] = [ 0       |k| ] [ η_k ]
      [ φ_k ]   [ -G_k     0 ] [ φ_k ],

[η_k(t)]   [ cos(ωt)        (|k|/ω) sin(ωt) ] [η_k(0)]
[φ_k(t)] = [ -(G_k/ω) sin(ωt)  cos(ωt)     ] [φ_k(0)].
```

Rescaling `φ_k` to `p_k=φ_k/G_k=η̇_k/ω²` converts this to the symmetric-looking
`ω·sin`/`sin/ω` matrix used in the next subsection. This change of variables explains why
papers and recovered listings may show `|k|/ω` (and `g/ω` in the subtracted second row) while
the reconstructed CPU operators expose `ω` and `1/ω`; the state-buffer contract, not the symbol
name `slope`, determines which coefficient is correct. The recovered listing's coefficients are
an audit reference, whereas the equations in the Local CPU implementation section intentionally
describe the checked-in C++ behavior.

The decompiled listing also adds a small `ε=10⁻⁶` to its computed `ω` and does not take the
checked-in C++ `|k|=0` branch. Its DC limit therefore keeps the second-row factor near `g·t`
(`sin(εt)/ε`), while the reconstructed CPU file stores both sine terms as zero at DC. In
addition, the listing uses `g/ω` in its second row even though its `ω` includes the cubic term,
so it is a recovered gravity--capillary hybrid rather than the fully consistent `G_k/ω` raw-
potential coefficient above. These are observable compatibility behaviors, not differences in
the continuum equation; use the direct source links when a bit-for-bit port is required.

#### 3.3 Exact modal propagator and state normalization

For gravity-only deep water, one can define `p_k=φ_k/g`. With the source's capillary-like
term, the general restoring-normalized choice is instead
`p_k=φ_k/(g+(σ/ρ)|k|²)=η̇_k/ω²`; this keeps the same matrix form. The state then obeys

```text
d/dt [ η_k ] = [ 0       ω² ] [ η_k ]
      [ p_k ]   [ -1      0 ] [ p_k ].
```

Call the matrix `A_k`. It satisfies `A_k²=-ω²I`, so its exponential terminates in two terms:

```text
exp(A_k t) = I cos(ωt) + (A_k/ω) sin(ωt)

[η_k(t)]   [ cos(ωt)        ω sin(ωt) ] [η_k(0)]
[p_k(t)] = [-sin(ωt)/ω      cos(ωt)   ] [p_k(0)].
```

This is the eWave exact exponential solver ([EW14](https://jtessen.people.clemson.edu/reports/papers_files/ewavealgorithm.pdf), Eqs.4--20).
Its normalized matrix has the same cosine and sine-over-frequency structure as the reconstructed
Local operator arrays (`operatorCos`, `operatorSinOmega`, and `operatorSinInvOmega`). The Local CPU
implementation section records a coefficient difference in the decompiled listing. In the
checked-in CPU implementation, `initialX` is the height/displacement-like channel and `initialY`
is interpreted for the displayed matrix as a restoring-normalized potential/velocity channel
(not literal `φ/g` when the cubic capillary term is active). This is a mathematical state
interpretation of the checked-in coefficient form, not a claim that the public `velocity
potential` payload is already normalized. The historical `m_slope` name does not mean a spatial
derivative.

Without damping, the normalized quadratic oscillator invariant

```text
E_k = 1/2 ( |η_k|² + ω² |p_k|² )
```

is invariant (up to floating-point error), because the propagator is the exact rotation in the
scaled coordinates `(η,ωp)`. It is a normalized state invariant, not a claim about the complete
physical per-mode water-wave energy. WaveWorks multiplies the operator by `a(t)=0.95^t/N²`; the
`N²` compensates the unnormalized inverse FFT, while `0.95^t` is a phenomenological amplitude
decay. If both state channels were scaled together, this invariant would decay approximately as
`0.95^(2t)`, not as `0.95^t`. In the actual feedback path only `initialX` receives the edge fade
and `initialY` is copied from `slope` unchanged, so the simple decay law applies only before that
asymmetric feedback step. Edge fade and feedback are SDK behavior, not consequences of the Airy
PDE.

#### 3.4 Damping, invariants, and model limits

Without the external SDK multiplier, the normalized modal propagator preserves the quadratic
oscillator invariant shown in §3.3. The factor `0.95^t` is a recovered phenomenological amplitude
decay, not a viscous term derived from the Airy equations. Border fade, feedback, and output
multipliers are likewise implementation behavior and are specified in Chapter 7.

### 4. Stochastic wind-spectrum theory and `S(k)`

#### 4.1 Covariance, PSD, and Gaussian Fourier coefficients

For a statistically homogeneous surface, the covariance depends only on separation,
`C(r)=E[η(x)η(x+r)]`. The Wiener--Khinchin theorem says that `C` and the power spectral density
`P_h(k)` are Fourier pairs (the graphics-facing form is Tessendorf's Eq.22; the theorem is the
Wiener/Khinchin stationary-process result). A finite periodic realization samples that PSD with
independent standard normal variables:

```text
h₀(k) = (ξ_r(k)+iξ_i(k))/sqrt(2) · sqrt(P_h(k))
ξ_r,ξ_i ~ N(0,1).
```

The `1/sqrt(2)` makes `E|h₀(k)|²=P_h(k)` under the usual per-coefficient convention. Tessendorf's
pair evolution adds the `k` and `-k` contributions, so the variance of the resulting real field
contains both members of the pair; do not compare a single coefficient's power with the pair power
without accounting for that factor. If `P_h` is a continuum density, the finite-cell coefficient
additionally contains `sqrt(Δkx Δky)` (and possibly `N²/(2π)` depending on the Fourier-series
convention). Those constants are not universal: Tessendorf's Eq.25 and WaveWorks' calibrated
amplitude multipliers choose different bookkeeping.

One way to make the bookkeeping explicit is to use the continuum convention

```text
η(x) = ∫ ĥ(k) exp(i k·x) d²k/(2π)²,
E[ĥ(k) conjugate(ĥ(k'))] = (2π)² δ(k-k') Φ(k).
```

On a periodic area `A=LxLy`, the Fourier-series coefficient is
`h_k=(1/A)∫_A η(x)exp(-i k·x)dx`. With the convention above,
`E|h_k|²≈Φ(k)/A = Φ(k)ΔkxΔky/(2π)²`, because
`ΔkxΔky=(2π)²/A`. The unnormalized grid DFT stores `H_k=N²h_k`, so
`E|H_k|²≈N⁴Φ(k)/A`. If a paper defines its PSD without the `(2π)²` factors, the displayed
factors move accordingly. This is why a paper's density, a Fourier-series coefficient, and the
source's calibrated `H0` amplitude must be named separately before comparing numbers; changing
the transform normalization without changing the random amplitude produces an `N²`-scale error.

The recovered Wind implementation's exact amplitude is

```text
h₀(k) = ξ(k) · [ a_base sqrt(S_base(k)) + a_swell sqrt(S_swell(k)) ],
```

where one deterministic Gaussian table supplies `ξ`. It is **not** generally
`ξ·sqrt(S_base+S_swell)` and it does not contain an implicit `1/sqrt(2)` in the return value.
`a_base` and `a_swell` include `amplitudeMultiplier/physicalSize`. The DC coefficient is forced to
zero. This distinction matters when estimating variance or changing the random-number table.

#### 4.2 From frequency-direction density to Cartesian `S(kx,ky)`

Ocean measurements often report energy per angular frequency and direction, `E_ω(ω,θ)`, so that
`E_ω dω dθ` is the energy in a small bin. A Cartesian wave-number PSD `S_k(kx,ky)` obeys

```text
S_k d²k = E_ω dω dθ,
d²k = k dk dθ,
dω = c_g dk,
```

and therefore

```text
S_k(k,θ) = E_ω(ω(k),θ) · c_g(k) / k.
```

If the measured spectrum is per ordinary frequency `f=ω/(2π)`, then
`df=c_g dk/(2π)` and the corresponding factor is `c_g/(2πk)`. One-sided versus two-sided
frequency spectra and the DFT's `N²` normalization add further `2` and `2π` factors. The CPU
code's `(groupVelocity*19.73921)/(2*waveNumber)` is consequently an implementation normalization
(`19.73921≈2π²`), not a universal JONSWAP constant. Preserve it for SDK compatibility and do not
silently replace it with `c_g/k` when comparing output amplitudes.

For the source dispersion `ω²=gk+a k³`, differentiating gives

```text
c_g = dω/dk = (g + 3a k²)/(2ω),      a=0.074/1000.
```

This is the exact group-velocity expression used by `Jonswap`; it explains why the derivative of
the cubic term appears in the frequency-to-wavenumber measure conversion. [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L41-L48)

#### 4.3 Phillips equilibrium tail and JONSWAP peak

The Phillips equilibrium range predicts a high-frequency angular spectrum proportional to
`α g² ω⁻⁵` (or a `k⁻⁴` wavenumber tail after the dispersion conversion in the
gravity-dominated deep-water regime). Wind pressure transfer
is discussed in [P57](https://doi.org/10.1017/S0022112057000233), while the equilibrium-tail
scaling is the subject of [P58](https://doi.org/10.1017/S0022112058000550). JONSWAP adds a narrow
fetch-dependent peak to that tail:

```text
S_J(ω) = α g² ω^-5 exp[-5/4 (ωp/ω)^4] γ^r,
r      = exp[-(ω-ωp)^2 / (2σ²ωp²)].
```

The original North Sea measurements ([J73](https://resolver.tudelft.nl/uuid:f204e188-13b9-49d8-a6dc-4fb7c20562fc)) motivate fetch-dependent `α`, peak frequency `ωp`, enhancement `γ`, and the asymmetric peak width. WaveWorks' recovered conversion is:

```text
ωp = 22 · (96.236107/(U F))^0.33
α  = 0.076 · (U²/(F g))^0.22
σ  = 0.07 for ω < ωp,  0.09 for ω ≥ ωp   (source comparison).
```

The source guards non-positive `U` and `F`, bounds exponential arguments at `16`, and uses a
rational approximation to `exp(-x)` for `x<0.25`. Those substitutions preserve the broad model
but are not part of the JONSWAP paper. The code also evaluates base wind and swell independently,
then combines their *amplitudes* after taking square roots.

#### 4.4 Directional spreading

Donelan, Hamilton, and Hui measured a frequency-dependent directional distribution that is well
represented by a normalized squared hyperbolic secant:

```text
D(δ;β) = β sech²(βδ) / [2 tanh(πβ)],
δ = wrap(θ-θ_w, -π, π).
```

Equivalently, if `Z(β)` denotes the integral of `sech²(βδ)` over `[-π,π]`, then
`Z(β)=2 tanh(πβ)/β` and `D=sech²(βδ)/Z(β)`.

The commonly quoted Donelan--Hamilton--Hui fit has a low-frequency plateau (`β=1.24` for
roughly `r≤0.56`), then the two power-law branches and a high-frequency plateau. The recovered
CPU code omits that `0.56` clamp and applies `2.61 r^1.3` for every `r<0.95`:

```text
β = 2.61 r^1.3,       r < 0.95
    2.28 r^-1.3,      0.95 ≤ r < 1.6
    1.24,             r ≥ 1.6,
r = ω/ωp.
```

The recovered CPU code follows the displayed extrapolated branches, approximates `sech²` with a polynomial reciprocal,
approximates `Z` with a rational function, and blends it with an isotropic term:

```text
D_source = windDependency · D(δ;β)
           + (1-windDependency)/(2π).
```

That blend is a WaveWorks control, not a claim that all JONSWAP measurements have an isotropic
component. [D85](https://doi.org/10.1098/rsta.1985.0054) is the source for the empirical branches;
the exact polynomial coefficients are recovered implementation details. [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L117-L160)

More specifically, with `x=πs_c`, the source approximates `tanh(x)` by

```text
T(x) ≈ [(17325x² + 135135 + 378x⁴ + x⁶)x]
       /[62370x² + 135135 + 3150x⁴ + 28x⁶],
```

and uses `s_c·windDependency·sech²(s_c·δ)/(2T(x))`. Its `sech²` path evaluates a truncated cosh
polynomial (`1+x²/2+x⁴/24+x⁶/720+x⁸/40320`) and takes the reciprocal twice. These are fast
single-precision approximations to the normalized analytical distribution, not additional
physical parameters. [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L129-L160)

#### 4.5 Cutoff and cascade window

The wavelength of a mode is `λ_w=2π/k`. For each base/swell component the CPU applies

```text
C(k) = 1-cutoffPower,  if cutoffLength > 2π/k,
       1,               otherwise.
```

The radial cascade window is a separate discrete mask on the integer grid radius. It prevents
adjacent cascades from heavily duplicating the same band and limits high-frequency aliasing. It is
not a smooth partition of unity: at a boundary, a sample can be hard-zeroed. This explains small
amplitude discontinuities when changing `lowerGridRadius` or `upperGridRadius`.

#### 4.6 Reference `S(k)` assembly and Gaussian `H0`

Combining the preceding radial, measure, directional, and cutoff terms gives the reference
component density and coefficient:

```text
S_c(k,θ) = P_c(ω) · R(k) · D_c(θ-θc) · cutoff_c(k)
h₀(k)    = (ξr+iξi)/sqrt(2) · sqrt(S_c(k))
```

The `1/sqrt(2)` is a per-coefficient variance convention. The real surface receives both `k`
and `-k` branches, so pair power must not be confused with the power of one coefficient. The
recovered CPU implementation uses a calibrated one-noise base/swell root-sum; that specialization
is documented in §7.5.2. [T01, Eq.25](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf)

## Part III — Spectral evolution and derived algorithms

### 5. Spectral evolution and displacement

#### 5.1 Wind conjugate-pair evolution

For a real surface on an ideal modular grid (and for non-duplicate endpoint pairs in the
recovered table), initialize a pair `h₀(k)` and `h₀(-k)` and evolve it as Tessendorf's Eq.26:

```text
H_t(k) = h₀(k) exp(+iωt) + conjugate(h₀(-k)) exp(-iωt).
```

Taking the conjugate and replacing `k` by `-k` proves
`H_t(-k)=conjugate(H_t(k))` for that modular pairing. Let `A=h₀(k)=A_r+iA_i`, `B=h₀(-k)=B_r+iB_i`, and
`c=cos(ωt), s=sin(ωt)`. Expanding both exponentials gives

```text
Re H_t = (A_r+B_r)c - (A_i+B_i)s
Im H_t = (A_r-B_r)s + (A_i-B_i)c.
```

`UpdateHt` computes exactly these four sums/differences. The Gaussian samples in `H0` are not
themselves copied into conjugate locations; they may be independent. It is the pair expression
above that imposes Hermitian symmetry at evolution time. [T01, Eq.26](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf)
and [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L725-L792)

#### 5.2 Spectral differentiation theorem

Differentiate one Fourier term:

```text
∂x [h̃(k)e^{ik·x}] = i kx h̃(k)e^{ik·x}.
```

Equivalently, for a periodic Fourier integral, integration by parts gives

```text
∫_0^L (∂xη)e^{-ikx} dx
 = [ηe^{-ikx}]_0^L + i k ∫_0^L ηe^{-ikx} dx
 = i k η̃(k),
```

because the boundary term vanishes when `η(0)=η(L)`. Therefore
`F{∂xη}=ikxH` and `F{∂yη}=ikyH`; this is Tessendorf's Eq.20. It is exact for the periodic
band-limited field and usually more accurate than finite differences for smooth waves.

#### 5.3 Choppy horizontal displacement

Tessendorf's choppy displacement (Eq.29) first defines an unscaled inverse-gradient field:

```text
D(k) = -i (k/|k|) H(k),         k≠0,
D(0) = 0.
```

The division by `|k|` makes the displacement direction follow the wave vector while preserving
height amplitude; the spatial map applies `λ` to the inverse-transformed field to control crest
sharpening. In `float2=(real,imag)` source convention,
`(-H.imag,+H.real)` is `+iH`, so the recovered CPU formula has the opposite sign to the displayed
Tessendorf convention. A swap of array axes or the sign in the inverse-transform convention can
produce exactly that difference. A one-mode test (set only `H[k]`, inspect the phase of `Dx`) is
safer than assuming one sign is universally correct. [WWS-CPU](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L442-L450)
and [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L778-L786)

### 6. Derived end-to-end algorithms

#### 6.1 Local Waves algorithm

1. Accumulate accepted spatial disturbances into the persistent grid.
2. Apply `FFTrows` and `FFTcolumns` to the displacement and second state channel.
3. Apply the per-wave-number two-channel modal propagator and damping.
4. Form lateral/Riesz spectra from the evolved height.
5. Apply inverse transforms, pack displacement, and feed back the persistent state for the next
   kick.

The theory in §3 defines the ideal state transition. The checked-in C++ and archived pseudocode
coefficient contracts are both retained in §7.4 because they differ in observable details.

#### 6.2 Wind Waves algorithm

1. Build or reuse a Gaussian table and angular-frequency table for each cascade.
2. Evaluate the source-compatible directional spectrum and construct `H0` amplitudes.
3. Add the opposite-wave-vector conjugate branch to obtain `Ht(k,t)`.
4. Derive height/lateral spectra and run full inverse FFTs for each cascade.
5. Apply centered-grid correction and publish the packed cascade output/archive entry.

The statistical derivation is in §4 and the exact padded-table, phase-reduction, and amplitude
rules are in §7.5.

#### 6.3 Shared algorithm and solver differences

| Property | Local Waves | Wind Waves |
|---|---|---|
| Initial condition | Spatial disturbances | Random wind/swell spectrum |
| Evolution | Two-channel damped modal state | Conjugate-pair phase rotation |
| Spatial scale | One finite periodic domain | Multiple independent cascades |
| Final state | Inverse FFT plus feedback | Inverse FFT plus output/archive |

Both solvers follow `state → spectral evolution → lateral spectra → inverse 2-D FFT → packed
real-space displacement`; only the source of the state and the feedback policy differ.

## Part IV — Recovered CPU implementation

### 7. Recovered CPU implementation

#### 7.1 FFT signs, scaling, and normalization ABI

The recovered CPU kernel uses a deliberately different ABI contract:

```text
FFTcSIMD(level, data, +1)  = forward sign  (exp(-i 2π kn/N)), no scale
FFTcSIMD(level, data, -1)  = inverse sign  (exp(+i 2π kn/N)), no scale
IFFT1D(FFT1D(x))           = N · x
IFFT2D(FFT2D(x))           = N² · x
```

Written as explicit CPU operators, the same contract is:

```text
FFTrows_CPU(η)[kx,y]      = Σ_x η[x,y] exp(-i 2π kx x/N)
FFTcolumns_CPU(R)[kx,ky]  = Σ_y R[kx,y] exp(-i 2π ky y/N)
IFFTrows_CPU(R)[x,y]      = Σ_kx R[kx,y] exp(+i 2π kx x/N)
IFFTcolumns_CPU(F)[x,y]   = Σ_ky F[x,ky] exp(+i 2π ky y/N)
FFT2D_CPU                 = FFTcolumns_CPU ∘ FFTrows_CPU
IFFT2D_CPU                = IFFTrows_CPU ∘ IFFTcolumns_CPU
```

The two inverse operators above intentionally omit `1/N`; their composition is therefore
`N²·η` in two dimensions. The normalized equations in Chapter 1 are useful when comparing with
textbook notation, while this block is the recovered CPU definition.
The composition labels are conceptual: the CPU scheduler reverses the forward passes (X first,
then Y), and the two tensor-product inverse axes commute mathematically even though their memory
strides and task order differ.

There is no division in `FFTcSIMD`, either after the final butterfly or inside a stage. Local
places `1/N²` in its time-dependent operator amplitude (`0.95^t/N²`); Wind instead obtains its
absolute scale from the spectrum and `amplitudeMultiplier/physicalSize`. A caller porting the
algorithm to a library with a normalized inverse must remove the corresponding scale exactly
once. [CT65](https://doi.org/10.1090/S0025-5718-1965-0178586-1) describes the factorization;
[WWS-CPU](sdk/src/Simulation_Util.cpp#L198-L279) is the authoritative transform contract.

#### 7.2 Four-lane packing and exact FFT data movement

The recovered CPU helper stores complex values as interleaved `float2` (`real,imag`) samples in
aligned arrays. `FFTcSIMD` presents those samples to the butterfly as `float4` lane groups:
`data[2*i]` contains four real values and `data[2*i+1]` four imaginary values.
`FFT1DSIMD_X_4wide` packs four rows into SIMD lanes and `FFT1DSIMD_Y_4wide` does the
corresponding four-column packing. This is a layout optimization only: each lane still follows
the same scalar radix-2 transform; there is no cross-lane butterfly.

For transform level `L`, the CPU helper uses `n = 2^L` and first performs an in-place
bit-reversal permutation. At stage `s`, `span = 2^(s+1)` and `halfSpan = span/2`. For each
butterfly group and `j < halfSpan`, the twiddle angle is `sign * 2πj/span`, where `sign` is
negative for forward and positive for inverse. Rows and columns are partitioned into fixed-size
CPU tasks; each task performs bit reversal and all butterfly stages for its assigned lines, then
writes results back in row-major order. The helper computes `ceil(log₂ n)` but allocates its
temporary for the caller's original `n`; `FFTcSIMD` then indexes `2^level` elements. Passing a
non-power-of-two length is therefore unsafe/undefined, not a supported rounding mode. Public
parameter conversion must enforce a power-of-two size. [WWS-CPU](sdk/src/Simulation_Util.cpp#L281-L356)

The inverse sign and scale are deliberately kept separate. The CPU butterfly does not divide in
every stage; normalization is folded into the evolution/output scales or the final displacement
generation step.

The power-of-two requirement is not the only precondition of the fixed task partitioning. The
recovered Local and Wind schedulers derive counts with shifts such as `N>>5`, `N>>4`, and `N>>3`
and process fixed line blocks. Consequently, supported sizes must also be large enough and
divisible by the corresponding block widths (the public presets use such sizes); a tiny power of
two can otherwise leave lines unassigned. Enforce the public size constraints before submitting
work rather than treating the task counts as a general remainder-handling loop. [WWS-CPU](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L561-L675)
and [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L400-L584)

#### 7.3 CPU indexing, zero mode, and storage compatibility

The two CPU paths use related but not identical index conventions. Wind's omega/H0 tables are
filled with `q = index-N/2`, while Local operator generation computes a positive half and mirrors
it with `N-index-1`; Local operator application later wraps `index >= N/2` to `index-N`. Thus each
negative-side entry receives the preceding positive-side magnitude (`q=-1` receives the `q=0`
operator, and the Nyquist edge receives the `q=N/2-1` operator), rather than the mathematical
`|N-index|` magnitude. This is a recovered storage/compatibility rule, not a replacement for the
mathematical `(-q) mod N` map; preserve it for bit-for-bit output and use a one-mode test when
porting the table layout. [WWS-CPU](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L411-L452)
and [WWS-CPU](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L755-L814)

The angular frequency used by both systems includes the deep-water gravity--capillary term
recovered in the code. It is not a finite-depth correction (finite depth would introduce a
`tanh(|k|h)` factor):

```text
ω²(k) = g |k| + (0.074/1000) |k|³
      = g |k| + 7.4e-5 |k|³
ω(k) = sqrt(ω²(k))
```

`0.074 N/m` is the recovered surface-tension-like coefficient and `1000 kg/m³` is the
assumed water density. The source uses `0.074*k³/1000` in the Wind omega table and
`0.000074*k³` in the Local operator; the two forms are numerically equivalent. This is a
WaveWorks parameterization of the deep-water relation, not evidence that the simulation models
finite water depth. [WWS-CPU](sdk/src/Simulation_Util.cpp#L173-L196)

At `|k| = 0`, the height operator is initialized but inverse-radius is zero, so both lateral
components are explicitly zero. This avoids a `0/0` and prevents a DC component from moving
the domain laterally. Although the analytical limit `sin(ωt)/ω` can be taken as `t` as
`|k|→0`, the checked-in reconstructed Local C++ explicitly stores both sine terms as zero at
the DC sample. The archived pseudocode instead adds `ε=10⁻⁶` to `ω`, so its lower-left
`g·sin(ωt)/ω` term approaches `g·t`; these are two compatibility contracts, not one universal
DC rule. Wind likewise forces the DC H0 amplitude to zero.

#### 7.4 Local CPU implementation

The Local solver maps the modal theory to persistent CPU arrays and a per-kick state update.
The checked-in reconstruction and the archived pseudocode are shown as separate contracts.

##### 7.4.1 Disturbance accumulation and persistent state

The public CPU façade accumulates each accepted `(x,y)` sample directly into a dense,
double-buffered `N×N` array. Coordinates outside the domain are ignored, and in-range coordinates
are converted to integer cells by the source's cast/truncation rule (there is no bilinear scatter).
At `kick`, the CPU
implementation walks all `N²` cells, adds the displacement channel to `initialX.real` and the
velocity-potential channel to `initialY.real`, then clears every consumed cell. There is no
sparse-record compaction step in the CPU implementation; the full-grid walk is an intentional
recovered behavior. [WWS-CPU](sdk/src/Local_Waves_Simulation.cpp#L200-L218)
and [WWS-CPU](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L251-L303)

##### 7.4.2 Checked-in C++ recurrence

The Local CPU implementation keeps six complex grids:

```text
initialX, initialY       persistent input/state grids
height, slope            evolved vertical and oscillator-state spectra
displacementX, Y         lateral spectra
```

The two persistent grids carry disturbance displacement and a second oscillator channel. The
public disturbance payload names its second scalar a velocity potential, but the checked-in CPU
coefficient form can only be interpreted as a restoring-normalized channel after a state
rescaling. `slope` is a historical/recovered name for that second state; it is not
`∂η/∂x` or `∂η/∂y` until a spatial derivative operator is applied. For
each frequency sample the CPU path precomputes:

```text
A(t) = 0.95^t / N²,       t = Δt_kick in the public CPU path
C    = A(t) cos(ωt)
Sω   = A(t) ω sin(ωt)
S/ω  = A(t) sin(ωt) / ω
```

Operator application is component-wise:

```text
height.real = initialX.real · C + initialY.real · Sω
height.imag = initialX.imag · C + initialY.imag · Sω
slope.real  = initialY.real · C - initialX.real · (S/ω)
slope.imag  = initialY.imag · C - initialX.imag · (S/ω)
```

##### 7.4.3 Decompiled recurrence and compatibility differences

Source-fidelity note: the preceding checked-in C++ recurrence is implemented by the [checked-in
reconstructed CPU file](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L786-L803), which stores
`Sω = ω·sin(ωt)·A`. The [decompiled CPU pseudocode
artifact](https://github.com/HuazyYang/waveworks2/blob/4d69707/src/_generic_pseudo/_Local_Waves_FFT_Simulation_CPU_Impl.cpp#L1675-L1679) stores its first
off-diagonal coefficient as `(|k|/ω)·sin(ωt)·A` and stores a
`(g/ω)·sin(ωt)·A` magnitude for the second term, which the state update subtracts. Treat those
as two separately audited contracts until a parity test resolves the state-channel normalization;
do not silently substitute one for the other when reproducing archived output. The modal
derivation in Chapter 3 explains the `ω`/`1/ω` form when `initialY` is interpreted as the
restoring-normalized state `p=η̇/ω²`.

For completeness, the recovered pseudocode contract can be written independently of the
checked-in reconstruction as follows. During table generation, `r,c` are non-negative indices in
the first half (`0 ≤ r,c < N/2`); the operator consumer later interprets its stored entries as
signed/centered indices. Here `t` is the Local kick interval, and the matrix acts on real and
imaginary lanes separately:

```text
k       = (2π/L) · sqrt(r² + c²)
ω̃       = sqrt(g·k + 7.4e-5·k³) + ε,     ε = 1e-6
a       = 0.95^t / N²
c       = a · cos(ω̃t)
b       = a · (k/ω̃) · sin(ω̃t)
d       = a · (g/ω̃) · sin(ω̃t)

[ H(t) ]   [ c   b ] [ X0 ]
[ P(t) ] = [ -d  c ] [ Y0 ]
```

That listing computes the positive half of the operator table and copies each coefficient triple
to the visual mirrors `(r,c)`, `(r,N-c-1)`, `(N-r-1,c)`, and `(N-r-1,N-c-1)`. This is a
recovered storage rule, not the modular identity `q↦(-q) mod N`; preserve it for archived-output
parity. It also uses a range-reduced polynomial sine/cosine implementation. The checked-in C++
path instead has an explicit zero-wave-number branch, uses `ω·sin` and `sin/ω`, and calls the
standard C++ trigonometric functions. The two blocks therefore describe distinct numerical
contracts until a source-level parity fix is made. [WWS-CPU](https://github.com/HuazyYang/waveworks2/blob/4d69707/src/_generic_pseudo/_Local_Waves_FFT_Simulation_CPU_Impl.cpp#L1462-L1803)

The off-diagonal ratio is a useful state-unit diagnostic: the checked-in form has
`Sω/(S/ω)=ω²`, the pseudocode form has `b/d=|k|/g`, and a physically consistent raw
gravity--capillary pair would have `(|k|/ω)/(G_k/ω)=|k|/G_k`. Thus the pseudocode agrees with
the raw-potential equations in the gravity-only limit, but is a deliberate gravity--capillary
hybrid once the cubic term is enabled. The public field name `velocity potential` alone does not
identify which normalization a buffer uses; preserve the audited coefficients.

##### 7.4.4 Lateral spectra, inverse transform, fade, and feedback

The lateral spectra use the evolved height and centered integer wave vector. In the repository's
`float2=(real,imag)` convention, the pair below is the source's orientation/sign convention:

```text
displacementX = (-kx · height.imag / |k|,
                  kx · height.real / |k|)
displacementY = (-ky · height.imag / |k|,
                  ky · height.real / |k|)
```

Algebraically this is `+i (kx/|k|)H` and `+i (ky/|k|)H` when `H=height.real+i·height.imag`.
Tessendorf's common equation writes `-i(k/|k|)H`; the difference is a coordinate/orientation
choice (and may be an array-axis convention), so validate a port with a single non-zero Fourier
mode rather than changing the recovered sign. [T01](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf)
and [WWS-CPU](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L431-L450)

After the inverse transforms, the CPU path applies amplitude/lateral multipliers and edge
attenuation. For border distance `d` and `b = 0.1N`, the fade is `1` for `d >= b`; otherwise:

```text
fade = exp(-5 · ((b-d)/b)² · Δt_kick)
```

The public `Local_Waves_Simulation::kick` passes its configured `m_deltaTime` as this argument;
the CPU member is named `m_simulationTime` but is not necessarily accumulated wall-clock time.
The faded height complex value is copied back into `initialX`, while `initialY` is replaced by
the slope field. This feedback lets a new disturbance be added to the residual wave state on
the next kick instead of restarting from the original impulse field. The checked-in C++ path
also applies the edge fade to the packed vertical value; the archived pseudocode packs vertical
as `amplitudeMultiplier·Re(H)` without that fade, packs laterals as
`lateralMultiplier·fade·Re(Dx/Dy)`, and feeds back `initialX=fade·H`, `initialY=P`. Keep this
output asymmetry in a parity test rather than treating the fade as part of the continuum model.

#### 7.5 Wind CPU implementation

Wind keeps one padded spectral state per cascade. Its implementation order is Gaussian/omega
table → source-compatible spectrum → pair evolution → three inverse planes → output packing.

##### 7.5.1 Gaussian and omega tables

`Simulation_Util::initGaussData` creates a deterministic `513×513` Gaussian table with row
stride 516. The Gaussian uses the repository's xorshift-128 state and Box–Muller form
`sqrt(-2 ln U) cos(2πV)`. Resource initialization resets the generator; each cascade then indexes
a centered crop of its deterministically regenerated table into the padded CPU pass. This is
reproducible but can introduce cross-cascade correlation. The omega table stores both `ω` and
`theta = atan2(ky,kx)`
for each centered vector.

Base and swell energies are evaluated independently and then combined. The recovered JONSWAP
calculation performs these stages:

1. Convert wind speed `U` and fetch distance `F` to peak frequency and alpha:
   `ωp = 22 · (96.236107/(U·F))^0.33` and
   `α = 0.076 · (U²/(F·g))^0.22`.
2. Apply peak enhancement with width `σ = 0.07` below the peak and `0.09` at/above it. The peak
   exponent is bounded for numerical stability before raising the configured peaking factor.
3. Evaluate the `ω^-5` Phillips term and the frequency-to-wavenumber measure factor. Directional
   spreading wraps the angle difference to `[-π,π]`, uses a frequency-dependent spread (1.24 by
   default, with recovered power laws below 1.6 and 0.95 of peak), and a normalized cosh-like
   approximation.
4. Apply wind dependency, the small-wave cutoff (`1-cutoffPower` outside the cutoff), and the
   base/swell amplitude scale (`amplitudeMultiplier/physicalSize`).
5. Take the square root of each component separately, apply its amplitude scale, add the two
   amplitudes, and multiply the result by the one Gaussian sample. This is not
   `sqrt(Sbase+Sswell)`; the distinction affects variance when both components are non-zero.

Samples outside `[lowerGridRadius, upperGridRadius)` are zeroed, as is the DC sample. CPU Wind
uses an `(N+1)×(N+4)` allocation with an active `(N+1)²` region and three stride-padding columns;
the padding columns are not spectral samples. The CPU path writes the active region;
`UpdateHt` reads the partner at `row=N-row`, `column=N-column` from that same padded table;
it does not keep a separate half-spectrum representation. [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L651-L776)

##### 7.5.2 Source-compatible JONSWAP and `H0` construction

For Wind Waves, the *component PSDs* are evaluated per cascade and per centered wave vector
`k=(kx,ky)`. As a conceptual decomposition (not the final source amplitude), write:

```text
S_components(k) = W(k) · [ Sbase(|k|, θ) + Sswell(|k|, θ) ]
```

This is a conceptual component-energy decomposition. The source's one-noise amplitude is instead

```text
A_eff(k) = W(k) · [ a_base sqrt(Sbase(k)) + a_swell sqrt(Sswell(k)) ]
H0(k)    = ξ(k) · A_eff(k)
```

so its effective variance is `A_eff²`, including a cross term; do not infer
`H0=ξ·sqrt(S_components)` without making an additional independent-noise assumption.

where `θ = atan2(ky,kx)` and `W(k)` is the cascade radial band-pass window:

```text
W(k) = 1,  lowerGridRadius ≤ sqrt(kxIndex² + kyIndex²) < upperGridRadius
       0,  otherwise
```

For component `c` (base or swell), define `U_c` as wind speed, `F_c` as fetch/distance,
`α_c` as the Phillips/JONSWAP amplitude, `ωp_c` as the peak angular frequency, and `γ_c` as
the configured spectrum-peaking value:

```text
ωp_c = 22 · (96.236107 / (U_c · F_c))^0.33
α_c  = 0.076 · (U_c² / (F_c · g))^0.22
ω    = sqrt(g·|k| + 7.4e-5·|k|³)
σ_c  = 0.07, ω <  ωp_c
       0.09, ω ≥ ωp_c
peak_c = exp(-(ω-ωp_c)² / (2·σ_c²·ωp_c²))
```

The branch above is the recovered CPU comparison (`ωp_c <= ω` selects `0.09`); many JONSWAP
references state the boundary using `≤` on the lower-frequency branch, so the equality case is
an implementation-level detail. [J73](https://resolver.tudelft.nl/uuid:f204e188-13b9-49d8-a6dc-4fb7c20562fc)
and [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L41-L56)

The radial JONSWAP/Phillips factor is:

```text
P_c(ω) = α_c · g² · ω^-5 · exp(-1.25 · (ωp_c/ω)^4)
         · γ_c^peak_c
```

The recovered implementation multiplies this by a frequency-to-wavenumber measure conversion.
Written explicitly, with

```text
c_g = 0.5 · (g + 3·0.074·|k|²/1000) / ω
R   = (c_g · 19.73921) / (2·|k|)
```

the energy before directional spreading is `P_c(ω)·R`. The `|k|=0` term is suppressed before
this division.

Directional spreading is centered on the component's wind angle `θc`. The angle difference is
wrapped to `δc ∈ [-π,π]`. The recovered CPU selects the following frequency-dependent spread
(it intentionally has no low-frequency `r=0.56` clamp):

```text
s_c = 2.61 · (ω/ωp_c)^1.3,  r < 0.95
     2.28 · (ω/ωp_c)^-1.3,  0.95 ≤ r < 1.6
     1.24,                  r ≥ 1.6,
     r = ω/ωp_c
```

The CPU evaluates the normalized directional factor with a reciprocal cosh approximation;
conceptually it is:

```text
D_c(δc) = [ s_c · windDependency_c · sech²(s_c·δc) / (2 tanh(π s_c)) ]
          + (1-windDependency_c)/(2π)
```

The canonical integral is
`∫_{-π}^{π}sech²(s_c·δ)dδ = 2 tanh(π s_c)/s_c`; the source's Pade-like `baseIntegral` approximates
`tanh(π s_c)` and uses it in the denominator. Finally, the small-wave cutoff multiplies
the component by `1-cutoffPower_c` when its cutoff length excludes the current wavelength:

```text
cutoff_c = 1-cutoffPower_c, wavelength < cutoffLength_c
           1,                    otherwise
S_c(|k|,θ) = P_c(ω) · R · D_c(θ-θc) · cutoff_c
```

The code uses the equivalent branch form (multiply by the complement only when the cutoff is
active), not a literal fractional exponent. `Sbase` and `Sswell` are converted to amplitudes and
multiplied by the complex Gaussian sample. With `a_base` and `a_swell` denoting the per-cascade
amplitude/size scales, the implementation-compatible form is:

```text
H0(k) = ξ(k) · [ a_base · sqrt(Sbase(k)) + a_swell · sqrt(Sswell(k)) ]
```

If desired, define an effective spectrum
`S_eff(k)=[a_base sqrt(Sbase(k))+a_swell sqrt(Sswell(k))]²`; this includes a cross term and is
generally not `Sbase+Sswell`. The exact constant (`sqrt(S)` versus `sqrt(S/2)`) depends on the
variance convention of the Gaussian table; the CPU path applies its scale factors explicitly
rather than relying on an implicit normalization.

The public parameter conversion supplies `F` in meters, converts the API's kilometer fetch
values before evaluation, and applies each cascade's amplitude multiplier divided by its
physical size. `UpdateH0` writes the active `(N+1)²` entries in a padded `(N+1)×(N+4)`
allocation. Gaussian values at opposite vectors are independent table entries; `UpdateHt` combines
`h0(k)` with the independently sampled `h0(-k)` and its algebraic conjugate, enforcing the
Hermitian pair for non-duplicate endpoints. The padded Nyquist-edge caveat is described in §7.5.3.
[WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L651-L718)

##### 7.5.3 `Ht` pair evolution and padded endpoints

`UpdateHt` uses the conjugate-pair recurrence. For an ideal modular pair (and for recovered
non-duplicate endpoint bins), let `h0(k)` and its mirrored sample be `h0(-k)`, with
`c = cos(ωt)` and `s = sin(ωt)`:

```text
sum  = h0(k) + h0(-k)
diff = h0(k) - h0(-k)
Ht.real = sum.real · c - sum.imag · s
Ht.imag = diff.real · s + diff.imag · c
```

The implementation reduces phase before `sin/cos` (the CPU uses double precision for the turn
calculation) to keep long-running simulations stable. The same pass computes lateral spectra
from `(-Ht.imag, Ht.real) * k/|k|`, so a separate gradient FFT is unnecessary.

##### 7.5.4 Inverse transforms, checkerboard correction, and output scaling

The CPU inverse path then runs a full complex FFT over all `N` rows and columns of each of the
three planes (height, lateral X, lateral Y). It does not combine frequencies in an `N/2` loop.
During output packing it multiplies every height and lateral sample by the checkerboard factor
`(-1)^(row+column)` and the recovered `displacementScale`. By default this is the API lateral
multiplier; when Beaufort scaling is enabled, the entrypoint replaces it with an interpolated
choppiness value. The checkerboard factor converts centered frequency layout into the output
array's origin order; omitting it produces an alternating-sign spatial image.
[WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L400-L436)
and [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L533-L584)

The checkerboard is a re-indexing correction, not a physical wave modulation. If a centered
array stores the coefficient for integer wave number `q` at index `j=q+N/2`, a natural-order
inverse transform receives an extra phase:

```text
exp(+i 2π (q+N/2)n/N) = (-1)^n exp(+i 2π qn/N).
```

In two dimensions the extra factor is `(-1)^(row+column)`. Multiplying by that factor during
output restores the spatial field associated with the centered origin. This explains why the
source applies the sign after the full inverse passes rather than adding an extra frequency
combine loop.

#### 7.6 CPU displacement output contract

The CPU numerical core ends after inverse-transforming height and lateral displacement and
packing the result. Spatial derivatives are still useful for understanding why the lateral
spectrum has its form:

```text
∂η/∂x ↔ i kx H(k)
∂η/∂y ↔ i ky H(k)
```

The recovered CPU path applies the Riesz/choppy multiplier directly to `H` and writes the three
displacement components; no additional gradient FFT is needed. The exact complex-sign convention
and zero-mode handling are documented in [WWS-CPU](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L431-L450)
and [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L725-L792).

## Part V — Architecture, verification, and provenance

### 8. CPU software architecture and execution pipeline

#### 8.1 Components, ownership, and lifecycle

The CPU implementation is organized as a public API façade, simulation orchestrators, CPU FFT
helpers, aligned complex arrays, and a thread-pool completion fence. Local owns one domain;
Wind owns four cascade states. Shape-changing properties wait for the completion fence before
recreating affected arrays. Parameter-only changes take effect when Local operators or Wind `H0`
are regenerated on the next kick. A kick publishes a monotonically increasing ID only after the
staging/readback buffers are complete.

CPU state consists of aligned complex arrays, disturbance double buffers, per-cascade Gaussian and
omega tables, FFT work arrays, output buffers, and archive history. Sampling is split into
thread-pool chunks; archive sampling interpolates neighboring completed kick IDs.

#### 8.2 Solver stages and task counts

```text
Local: dense disturbance → FFTrows → FFTcolumns → evolve → IFFTrows → IFFTcolumns → output/feedback
Wind: Gaussian/JONSWAP H0 → conjugate-pair Ht → lateral spectra → IFFTrows → IFFTcolumns → output/archive
```

#### 8.3 Continuation scheduling and task counts

The CPU implementations do not run the stages as one monolithic loop. They submit an initial
low-priority batch to `ThreadPool` and use atomic counters to enqueue continuation batches when
the final task in a stage finishes. This preserves the dependency graph while allowing rows,
columns, cascades, and displacement planes to run in parallel.

Local task counts are:

```text
forward X FFT       N/32
forward Y FFT       N/32
operator generation N/16
operator application N/8
reverse X FFT       N/32
reverse Y FFT       N/32
displacement/output packing N/8
```

Wind task counts per cascade are:

```text
H0 refresh          N/8 + 1       (first kick or changed spectrum)
Ht evolution       N/16
FFT X              3N/32          (height, lateral X, lateral Y planes)
FFT Y              3N/32
displacement/output packing N/16
```

The submitted task count includes all expected continuation stages, not just the first batch.
Consequently, `waitForLowPriorityWorkDone` is a completion fence for the whole numerical
pipeline. Worker timing is accumulated atomically; the manager records the main-thread wait
separately. A staging-cursor flip is legal only after that fence, which prevents a consumer or
sampler from observing a half-transformed buffer.

#### 8.4 Stage contracts, continuations, and completion fence

| Stage | Inputs | Outputs | Dependency/fence | CPU source |
|---|---|---|---|---|
| Local disturbance ingest | Public `(x,y,dx,velocity)` samples and dense write buffer | `initialX.real`, `initialY.real` additions; consumed cells cleared | Runs before the forward X pass | [`Local_Waves_Simulation.cpp#L200-L218`](sdk/src/Local_Waves_Simulation.cpp#L200-L218), [`Local_Waves_FFT_Simulation_CPU_Impl.cpp#L251-L303`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L251-L303) |
| Local forward transform | `initialX`, `initialY` | Frequency-domain state | X-line tasks complete before Y-line tasks | [`Local_Waves_FFT_Simulation_CPU_Impl.cpp#L561-L645`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L561-L645) |
| Local operator/evolution | Frequency state and per-bin operators | `height`, oscillator-state channel, lateral spectra | Both forward axes complete first | [`Local_Waves_FFT_Simulation_CPU_Impl.cpp#L411-L452`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L411-L452), [`#L755-L814`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L755-L814) |
| Local inverse/output | Four spectral planes | Packed displacement buffer and feedback state | Reverse X then reverse Y, then output rows | [`Local_Waves_FFT_Simulation_CPU_Impl.cpp#L589-L753`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L589-L753) |
| Wind H0 | Gaussian crop, omega table, base/swell parameters | Active `(N+1)²` H0 entries in a padded allocation | Runs on first kick or spectrum change | [`Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L586-L723`](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L586-L723) |
| Wind Ht/lateral | H0 pair and phase | Height, lateral-X, lateral-Y spectral planes | H0 completion precedes Ht; all Ht rows precede FFT X | [`Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L725-L792`](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L725-L792) |
| Wind inverse/output | Three spectral planes | FP16 displacement and optional float displacement map | Three plane X/Y transforms precede output packing | [`Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L400-L584`](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L400-L584) |

The manager's atomic counters are stage-completion counters, not work-stealing queues. The last
task in a stage enqueues the next vector of tasks; `waitForLowPriorityWorkDone` waits for all
continuations already submitted through that chain. This ordering is required by the tensor-product
FFT: lines in the next axis must not observe partially transformed data. [WWS-CPU](sdk/src/Wind_Waves_Simulation_Manager_CPU_Impl.cpp#L112-L149)
and [WWS-CPU](sdk/src/Wind_Waves_Simulation_Manager_CPU_Impl.cpp#L253-L341)

#### 8.5 Memory and performance consequences

The line transforms are independent only within a direction. All row FFT tasks must finish before
column FFT tasks consume their results; evolution must finish before inverse rows, and inverse
rows before inverse columns. The thread-pool counters encode this partial order. Four-lane packing
reduces arithmetic-loop overhead but the current reconstructed wrappers allocate `2N` temporary
`float4` values per line group, so allocation and strided Y gathers can dominate for small `N`.
These are performance properties of this implementation, not changes to the `O(N²logN)` theory.

#### 8.6 Source ownership anchors

- `src/Entrypoints.cpp` — public lifecycle and parameter conversion.
- `src/Local_Waves_Simulation.cpp` and `src/Wind_Waves_Simulation.cpp` — orchestration and cursors.
- `src/*_FFT_Simulation_CPU_Impl.cpp` — CPU FFT, evolution, output, and sampling.
- `src/Wind_Waves_Simulation_Manager_CPU_Impl.cpp` — cascade coordination.

### 9. Numerical behavior and verification

#### 9.1 Phase reduction and trigonometric approximations

Wind computes `turns=ω·t/(2π)` in double precision, truncates `(int)turns`, and converts the
fractional turn back to radians before calling a polynomial sine/cosine approximation. This is
different from `floor`/`fmod` for negative times and from a platform `sinf/cosf` at large angles.
The choice bounds the polynomial input and makes long positive runs less sensitive to loss of
low bits in `ωt`; it also means bitwise parity can change if a port uses a different cast or fast
math mode. The cast truncates toward zero (not mathematical floor); very large turn counts can
also exceed the representable `int` range, so a robust port should define its bounded-time policy
explicitly. [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L282-L335)

The same distinction matters for Local parity: the decompiled operator generator uses a
single-precision range-reduced polynomial, whereas the checked-in reconstructed CPU file calls
the C++ `std::sin`/`std::cos` functions. Compare within a numerical tolerance unless the exact
range-reduction sequence is intentionally restored.

#### 9.2 Round-off, normalization, and source-parity tests

The most useful CPU invariants are:

```text
1. IFFT(FFT(x)) ≈ N² x       (2-D, because FFTcSIMD is unnormalized).
2. H[-k] ≈ conjugate(H[k])    (for non-Nyquist-edge pairs after Wind Ht; ideal self-conjugate bins are real).
3. DC lateral displacement = 0.
4. One mode advances by phase ωΔt and satisfies the Local matrix recurrence.
5. Parseval: ||x||² ≈ ||X||²/N².
```

A tolerance should scale with `ε_machine log₂N` and the number of chained stages, consistent with
the usual backward-error analysis for FFTs [HIGHAM](https://doi.org/10.1137/1.9780898718027.ch24).
Repeated
complex multiplication of twiddles, half-angle square roots, float4 pack/unpack, FP16 output
conversion, and polynomial trig each add a different error source. Do not use an exact-energy
assertion after damping or edge fade; test the undamped operator separately.

The ideal Hermitian check needs one source-specific qualification: Wind stores duplicate `-N/2`
and `+N/2` endpoints in its padded `H0` table, and those two random entries are independent.
Consequently a Nyquist-edge imaginary residue can survive the pair algebra; the recovered output
uses the real lane. A strict mathematical port should force every self-conjugate output bin real
before the inverse transform. [WWS-CPU](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L651-L792)

### 10. Equation-to-source map

| Mathematical object or claim | Primary reference | Recovered CPU evidence |
|---|---|---|
| DFT signs, radix-2 butterfly, `O(N log N)` | [Cooley--Tukey 1965](https://doi.org/10.1090/S0025-5718-1965-0178586-1) | [`Simulation_Util.cpp#L198-L279`](sdk/src/Simulation_Util.cpp#L198-L279) |
| Multidimensional separability and signs | FFTW multidimensional DFT [manual](https://www.fftw.org/fftw3_doc/Multi_002ddimensional-Transforms.html) | [`Simulation_Util.cpp#L281-L356`](sdk/src/Simulation_Util.cpp#L281-L356) (recovered four-lane packing) |
| Deep-water and capillary dispersion | Tessendorf [Eqs.14--16](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf) | [`Simulation_Util.cpp#L173-L196`](sdk/src/Simulation_Util.cpp#L173-L196), [`Local_Waves_FFT_Simulation_CPU_Impl.cpp#L755-L814`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L755-L814) |
| Fourier surface expansion and slopes | Tessendorf [Eqs.19--20](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf) | [`Local_Waves_FFT_Simulation_CPU_Impl.cpp#L411-L452`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L411-L452) |
| Local oscillator propagator and state scaling | Tessendorf/eWave [Eqs.4--20](https://jtessen.people.clemson.edu/reports/papers_files/ewavealgorithm.pdf) | [`Local_Waves_FFT_Simulation_CPU_Impl.cpp#L755-L814`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L755-L814); decompiled coefficient variant is documented in the Local CPU implementation section |
| Dense disturbance accumulation and kick | Tessendorf interactive-wave model | [`Local_Waves_Simulation.cpp#L200-L218`](sdk/src/Local_Waves_Simulation.cpp#L200-L218), [`Local_Waves_FFT_Simulation_CPU_Impl.cpp#L251-L303`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp#L251-L303) |
| Phillips/JONSWAP radial spectrum | [Phillips 1958](https://doi.org/10.1017/S0022112058000550), [JONSWAP 1973](https://resolver.tudelft.nl/uuid:f204e188-13b9-49d8-a6dc-4fb7c20562fc) | [`Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L23-L280`](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L23-L280) |
| Directional spread | [Donelan et al. 1985](https://doi.org/10.1098/rsta.1985.0054) | [`Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L117-L160`](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L117-L160) |
| Gaussian `H0` initialization | Tessendorf [Eq.25](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf) | [`Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L586-L723`](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L586-L723), [`Simulation_Util.cpp#L139-L171`](sdk/src/Simulation_Util.cpp#L139-L171) |
| Conjugate-pair phase evolution | Tessendorf [Eq.26](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf) | [`Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L725-L792`](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L725-L792) |
| Choppy displacement | Tessendorf [Eq.29](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf) | [`Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L778-L786`](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp#L778-L786) |

### 11. References and provenance

#### 11.1 Source tags and their roles

| Tag | Source | Role in this page |
|---|---|---|
| `[CT65]` | Cooley & Tukey, *An Algorithm for the Machine Calculation of Complex Fourier Series* (1965), [DOI/PDF](https://doi.org/10.1090/S0025-5718-1965-0178586-1) | Radix-2 factorization, butterfly operation, in-place array organization, and `O(N log N)` cost. |
| `[T01]` | Jerry Tessendorf, *Simulating Ocean Water* (SIGGRAPH course notes, 2001), [author PDF](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf) | Eqs.14--16 (dispersion), 19--26 (Fourier/PSD/Gaussian evolution), and 29 (choppy displacement); source theory for periodic surface synthesis. |
| `[T04]` | Jerry Tessendorf, *Interactive Water Surfaces* (Game Programming Gems 4, 2004), [author PDF](https://jtessen.people.clemson.edu/reports/papers_files/Interactive_Water_Surfaces.pdf) | Interactive/local wave motivation and the damped iWave height PDE; the coupled state matrix is attributed to `[EW14]`. |
| `[P57]` | O. M. Phillips, *On the generation of waves by turbulent wind* (1957), [JFM DOI](https://doi.org/10.1017/S0022112057000233) | Physical mechanism by which turbulent pressure transfers energy from wind to waves. |
| `[P58]` | O. M. Phillips, *The equilibrium range in the spectrum of wind-generated waves* (1958), [JFM DOI](https://doi.org/10.1017/S0022112058000550) | Equilibrium high-frequency tail (`ω⁻⁵`, with `k⁻⁴` in the gravity-dominated deep-water limit) used by Phillips-type spectra. |
| `[J73]` | Hasselmann et al., *Measurements of Wind-Wave Growth and Swell Decay during the Joint North Sea Wave Project (JONSWAP)* (1973), [TU Delft resolver/report](https://resolver.tudelft.nl/uuid:f204e188-13b9-49d8-a6dc-4fb7c20562fc) | §2.4.3--2.4.5: fetch-dependent peak frequency/alpha, peak enhancement, and asymmetric peak width. |
| `[D85]` | Donelan et al., *Directional spectra of wind-generated waves* (1985), [Royal Society DOI](https://doi.org/10.1098/rsta.1985.0054) | Empirical `sech²` directional spreading and the `2.61/2.28/1.24` frequency-dependent spread. |
| `[EW14]` | Jerry Tessendorf, *eWave: Using an Exponential Solver on the iWave Problem* (2014), [author PDF](https://jtessen.people.clemson.edu/reports/papers_files/ewavealgorithm.pdf) | Mathematical lineage for the exact Fourier-space harmonic-oscillator propagator; state scaling is checked separately against the CPU source. |
| `[FFTW]` | FFTW3, [DFT sign/normalization](https://www.fftw.org/fftw3_doc/What-FFTW-Really-Computes.html) and [multidimensional transform](https://www.fftw.org/fftw3_doc/Multi_002ddimensional-Transforms.html) documentation | Independent reference for separability, row/column composition, and the unnormalized inverse convention. |
| `[HINCH]` | R. S. Hinch, [capillary--gravity wave notes](https://www.damtp.cam.ac.uk/user/hinch/teaching/D24c.pdf) | Finite-depth capillary--gravity dispersion used only as a comparison with the source's deep-water approximation. |
| `[HIGHAM]` | N. J. Higham, [FFT accuracy chapter](https://doi.org/10.1137/1.9780898718027.ch24) | Numerical-error context; not a claim about a particular WaveWorks implementation. |
| `[WWS-CPU]` | Reconstructed CPU implementation: [`Simulation_Util.cpp`](sdk/src/Simulation_Util.cpp), [`Local_Waves_FFT_Simulation_CPU_Impl.cpp`](sdk/src/Local_Waves_FFT_Simulation_CPU_Impl.cpp), and [`Wind_Waves_FFT_Simulation_CPU_Impl.cpp`](sdk/src/Wind_Waves_FFT_Simulation_CPU_Impl.cpp) | Exact constants, storage layout, task partitioning, damping/fade, padded tables, and floating-point operation order. |

#### 11.2 Primary theory and ocean-spectrum sources

1. J. W. Cooley and J. W. Tukey, “An Algorithm for the Machine Calculation of Complex Fourier
   Series,” *Mathematics of Computation* 19(90), 297--301 (1965),
   [DOI and AMS PDF](https://doi.org/10.1090/S0025-5718-1965-0178586-1).
2. W. M. Gentleman and G. Sande, “Fast Fourier Transforms: for Fun and Profit,” AFIPS (1966),
   [DOI](https://doi.org/10.1145/1464291.1464352). This is a useful alternative derivation of
   decimation-in-time/decimation-in-frequency data ordering.
3. M. Frigo and S. G. Johnson, “The Design and Implementation of FFTW3,” *Proceedings of the
   IEEE* 93(2), 216--231 (2005), [DOI](https://doi.org/10.1109/JPROC.2004.840301). It provides
   cache/planner context; it is not used as a claim that this repository calls FFTW.
4. J. Tessendorf, “Simulating Ocean Water,” SIGGRAPH course notes (2001--2004),
   [author PDF](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2002.pdf).
   Equations 14--16, 19--26, and 29 are the principal ocean-model anchors used here; the
   later geometric fold equations are outside this CPU-only page.
5. J. Tessendorf, “Interactive Water Surfaces,” *Game Programming Gems 4* (2004),
   [author PDF](https://jtessen.people.clemson.edu/reports/papers_files/Interactive_Water_Surfaces.pdf).
6. J. Tessendorf, “eWave: Using an Exponential Solver on the iWave Problem” (2014),
   [author PDF](https://jtessen.people.clemson.edu/reports/papers_files/ewavealgorithm.pdf).
7. O. M. Phillips, “On the generation of waves by turbulent wind,” *Journal of Fluid Mechanics*
   2 (1957), [DOI](https://doi.org/10.1017/S0022112057000233).
8. O. M. Phillips, “The equilibrium range in the spectrum of wind-generated waves,” *Journal of
   Fluid Mechanics* 4 (1958), [DOI](https://doi.org/10.1017/S0022112058000550).
9. K. Hasselmann et al., “Measurements of Wind-Wave Growth and Swell Decay during the Joint North
   Sea Wave Project (JONSWAP),” report (1973), [TU Delft resolver/PDF](https://resolver.tudelft.nl/uuid:f204e188-13b9-49d8-a6dc-4fb7c20562fc).
10. M. A. Donelan, J. Hamilton, and W. H. Hui, “Directional spectra of wind-generated waves,”
    *Philosophical Transactions of the Royal Society A* 315 (1985),
    [DOI](https://doi.org/10.1098/rsta.1985.0054).
11. G. A. Mastin, P. A. Watterberg, and J. F. Mareda, “Fourier Synthesis of Ocean Scenes,” *IEEE
    Computer Graphics and Applications* 7(3), 16--23 (1987), [DOI](https://doi.org/10.1109/MCG.1987.276961).

#### 11.3 Supporting numerical and hydrodynamics references

12. FFTW3 manual, “What FFTW Really Computes” and “Multi-dimensional Transforms,”
    [official documentation](https://www.fftw.org/fftw3_doc/What-FFTW-Really-Computes.html).
13. S. G. Johnson, “Fourier spectral differentiation,” [MIT note](https://math.mit.edu/~stevenj/fft-deriv.pdf),
    for the derivative multiplier and Nyquist/aliasing cautions.
14. MIT 2.016, “Free Surface Waves,” [hydrodynamics lecture note](https://ocw.mit.edu/courses/2-016-hydrodynamics-13-012-fall-2005/3133000c14ec08086951d724ff277b13_free_surf_wave.pdf),
    for the finite-depth gravity dispersion relation. For the gravity--capillary finite-depth
    extension, see [Hinch's capillary-wave notes](https://www.damtp.cam.ac.uk/user/hinch/teaching/D24c.pdf);
    the CPU source itself remains a deep-water parameterization.
15. N. J. Higham, “The Fast Fourier Transform and Applications,” in *Accuracy and Stability of
    Numerical Algorithms*, [FFT chapter](https://doi.org/10.1137/1.9780898718027.ch24), for
    round-off/error-growth context.
16. N. Wiener, “Generalized Harmonic Analysis,” *Acta Mathematica* 55 (1930),
    [DOI](https://doi.org/10.1007/BF02546511), and A. Khinchin, “Korrelationstheorie der
    stationären stochastischen Prozesse,” *Mathematische Annalen* 109 (1934),
    [DOI](https://doi.org/10.1007/BF01449156), the original Wiener--Khinchin covariance/PSD
    lineage. The graphics-facing statement used in this page is Tessendorf Eq.22.

#### 11.4 Repository implementation sources

The code links in this page point to the reconstructed CPU implementation. When a recovered
CPU pseudocode listing is useful for auditing operation order, consult only the matching CPU
artifacts: [`_Local_Waves_FFT_Simulation_CPU_Impl.cpp`](https://github.com/HuazyYang/waveworks2/blob/4d69707/src/_generic_pseudo/_Local_Waves_FFT_Simulation_CPU_Impl.cpp),
[`_Wind_Waves_FFT_Simulation_CPU_Impl.cpp`](https://github.com/HuazyYang/waveworks2/blob/4d69707/src/_generic_pseudo/_Wind_Waves_FFT_Simulation_CPU_Impl.cpp),
and [`_Wind_Waves_Simulation_Manager_CPU_Impl.cpp`](https://github.com/HuazyYang/waveworks2/blob/4d69707/src/_generic_pseudo/_Wind_Waves_Simulation_Manager_CPU_Impl.cpp).
These artifacts preserve padding, mirror indices, and approximation coefficients, but they are
implementation evidence rather than independent physical theory.
