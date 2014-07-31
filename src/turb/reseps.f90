!-------------------------------------------------------------------------------

! This file is part of Code_Saturne, a general-purpose CFD tool.
!
! Copyright (C) 1998-2014 EDF S.A.
!
! This program is free software; you can redistribute it and/or modify it under
! the terms of the GNU General Public License as published by the Free Software
! Foundation; either version 2 of the License, or (at your option) any later
! version.
!
! This program is distributed in the hope that it will be useful, but WITHOUT
! ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
! FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
! details.
!
! You should have received a copy of the GNU General Public License along with
! this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
! Street, Fifth Floor, Boston, MA 02110-1301, USA.

!-------------------------------------------------------------------------------

!===============================================================================
! Function:
! ---------

!> \file reseps.f90
!>
!> \brief This subroutine performs the solving of epsilon in
!>        \f$ R_{ij} - \varepsilon \f$ RANS turbulence model.
!>
!-------------------------------------------------------------------------------

!-------------------------------------------------------------------------------
! Arguments
!______________________________________________________________________________.
!  mode           name          role
!______________________________________________________________________________!
!> \param[in]     nvar          total number of variables
!> \param[in]     nscal         total number of scalars
!> \param[in]     ncepdp        number of cells with head loss
!> \param[in]     ncesmp        number of cells with mass source term
!> \param[in]     ivar          variable number
!> \param[in]     isou          local variable number (7 here)
!> \param[in]     ipp           index for writing
!> \param[in]     icepdc        index of cells with head loss
!> \param[in]     icetsm        index of cells with mass source term
!> \param[in]     itypsm        type of mass source term for each variable
!>                               (see \ref ustsma)
!> \param[in]     dt            time step (per cell)
!> \param[in,out] rtp, rtpa     calculated variables at cell centers
!>                               (at current and previous time steps)
!> \param[in]     propce        physical properties at cell centers
!> \param[in]     gradv         work array for the term grad
!>                               of velocity only for iturb=31
!> \param[in]     produc        work array for production (without
!>                               rho volume) only for iturb=30
!> \param[in]     gradro        work array for \f$ \grad{rom} \f$
!> \param[in]     ckupdc        work array for the head loss
!> \param[in]     smacel        value associated to each variable in the mass
!>                               source terms or mass rate (see \ref ustsma)
!> \param[in]     viscf         visc*surface/dist at internal faces
!> \param[in]     viscb         visc*surface/dist at edge faces
!> \param[in]     tslagr        coupling term for lagrangian
!> \param[in]     smbr          working array
!> \param[in]     rovsdt        working array
!_______________________________________________________________________________

subroutine reseps &
 ( nvar   , nscal  , ncepdp , ncesmp ,                            &
   ivar   , isou   , ipp    ,                                     &
   icepdc , icetsm , itypsm ,                                     &
   dt     , rtp    , rtpa   , propce ,                            &
   gradv  , produc , gradro ,                                     &
   ckupdc , smacel ,                                              &
   viscf  , viscb  ,                                              &
   tslagr ,                                                       &
   smbr   , rovsdt )

!===============================================================================

!===============================================================================
! Module files
!===============================================================================

use paramx
use numvar
use entsor
use optcal
use cstnum
use cstphy
use parall
use period
use lagran
use mesh
use field
use field_operator
use cs_f_interfaces

!===============================================================================

implicit none

! Arguments

integer          nvar   , nscal
integer          ncepdp , ncesmp
integer          ivar   , isou   , ipp

integer          icepdc(ncepdp)
integer          icetsm(ncesmp), itypsm(ncesmp,nvar)

double precision dt(ncelet), rtp(ncelet,nflown:nvar), rtpa(ncelet,nflown:nvar)
double precision propce(ncelet,*)
double precision produc(6,ncelet), gradv(3, 3, ncelet)
double precision gradro(ncelet,3)
double precision ckupdc(ncepdp,6), smacel(ncesmp,nvar)
double precision viscf(nfac), viscb(nfabor)
double precision tslagr(ncelet,*)
double precision smbr(ncelet), rovsdt(ncelet)

! Local variables

integer          iel
integer          iiun
integer          iflmas, iflmab
integer          nswrgp, imligp, iwarnp
integer          iconvp, idiffp, ndircp, ireslp
integer          nitmap, nswrsp, ircflp, ischcp, isstpp, iescap
integer          imgrp , ncymxp, nitmfp
integer          iptsta
integer          imucpp, idftnp, iswdyp
integer          icvflb
integer          ivoid(1)
double precision blencp, epsilp, epsrgp, climgp, extrap, relaxp
double precision epsrsp, alpha3
double precision trprod , trrij
double precision tseps , kseps , ceps2
double precision tuexpe, thets , thetv , thetap, thetp1
double precision prdeps, xttdrb, xttke , xttkmg

double precision rvoid(1)

double precision, allocatable, dimension(:) :: w1
double precision, allocatable, dimension(:) :: w7, w9
double precision, allocatable, dimension(:) :: dpvar
double precision, allocatable, dimension(:,:) :: viscce
double precision, allocatable, dimension(:,:) :: weighf
double precision, allocatable, dimension(:) :: weighb
double precision, dimension(:), pointer :: imasfl, bmasfl
double precision, dimension(:), pointer ::  crom, cromo
double precision, dimension(:), pointer :: coefap, coefbp, cofafp, cofbfp
double precision, dimension(:,:), pointer :: visten
double precision, dimension(:), pointer :: cvara_ep, cvar_al
double precision, dimension(:), pointer :: cvara_r11, cvara_r22, cvara_r33
double precision, dimension(:), pointer :: cvara_r12, cvara_r13, cvara_r23
double precision, dimension(:), pointer :: viscl, visct

character(len=80) :: label

!===============================================================================

!===============================================================================
! 1. Initialization
!===============================================================================

! Allocate work arrays
allocate(w1(ncelet))
allocate(w9(ncelet))
allocate(dpvar(ncelet))
allocate(viscce(6,ncelet))
allocate(weighf(2,nfac))
allocate(weighb(nfabor))

if(iwarni(ivar).ge.1) then
  call field_get_label(ivarfl(ivar), label)
  write(nfecra,1000) label
endif

call field_get_val_s(icrom, crom)
call field_get_val_s(iprpfl(iviscl), viscl)
call field_get_val_s(iprpfl(ivisct), visct)

call field_get_val_prev_s(ivarfl(iep), cvara_ep)
if (iturb.eq.32) call field_get_val_s(ivarfl(ial), cvar_al)

call field_get_val_prev_s(ivarfl(ir11), cvara_r11)
call field_get_val_prev_s(ivarfl(ir22), cvara_r22)
call field_get_val_prev_s(ivarfl(ir33), cvara_r33)
call field_get_val_prev_s(ivarfl(ir12), cvara_r12)
call field_get_val_prev_s(ivarfl(ir13), cvara_r13)
call field_get_val_prev_s(ivarfl(ir23), cvara_r23)

call field_get_key_int(ivarfl(iu), kimasf, iflmas)
call field_get_key_int(ivarfl(iu), kbmasf, iflmab)
call field_get_val_s(iflmas, imasfl)
call field_get_val_s(iflmab, bmasfl)

call field_get_coefa_s(ivarfl(ivar), coefap)
call field_get_coefb_s(ivarfl(ivar), coefbp)
call field_get_coefaf_s(ivarfl(ivar), cofafp)
call field_get_coefbf_s(ivarfl(ivar), cofbfp)

! Constant Ce2, which worths Ce2 for iturb=30 and CSSGE2 for itrub=31
if (iturb.eq.30) then
  ceps2 = ce2
elseif (iturb.eq.31) then
  ceps2 = cssge2
else
  ceps2 = cebme2
endif

! S as Source, V as Variable
thets  = thetst
thetv  = thetav(ivar )

if (isto2t.gt.0.and.iroext.gt.0) then
  call field_get_val_prev_s(icrom, cromo)
else
  call field_get_val_s(icrom, cromo)
endif
if(isto2t.gt.0) then
  iptsta = ipproc(itstua)
else
  iptsta = 0
endif

do iel = 1, ncel
  smbr(iel) = 0.d0
enddo
do iel = 1, ncel
  rovsdt(iel) = 0.d0
enddo

!===============================================================================
! 2. User source terms
!===============================================================================

call cs_user_turbulence_source_terms &
!===================================
 ( nvar   , nscal  , ncepdp , ncesmp ,                            &
   ivarfl(ivar)    ,                                              &
   icepdc , icetsm , itypsm ,                                     &
   ckupdc , smacel ,                                              &
   smbr   , rovsdt )

!     If we extrapolate the source terms
if(isto2t.gt.0) then
  do iel = 1, ncel
    !       Save for exchange
    tuexpe = propce(iel,iptsta+isou-1)
    !       For the continuation and the next time step
    propce(iel,iptsta+isou-1) = smbr(iel)
    !       Second member of previous time step
    !       We suppose -rovsdt > 0: we implicit
    !          the user source term (the rest)
    smbr(iel) = rovsdt(iel)*rtpa(iel,ivar) - thets*tuexpe
    !       Diagonal
    rovsdt(iel) = - thetv*rovsdt(iel)
  enddo
else
  do iel = 1, ncel
    smbr(iel)   = rovsdt(iel)*rtpa(iel,ivar) + smbr(iel)
    rovsdt(iel) = max(-rovsdt(iel),zero)
  enddo
endif

!===============================================================================
! 3. Lagrangian source terms
!===============================================================================

!     Second order is not taken into account
if (iilagr.eq.2 .and. ltsdyn.eq.1) then

  do iel = 1, ncel
    ! Source terms with eps
    tseps = -0.5d0 * ( tslagr(iel,itsr11)                        &
                     + tslagr(iel,itsr22)                        &
                     + tslagr(iel,itsr33) )
    ! quotient k/eps
    kseps = 0.5d0 * ( cvara_r11(iel)                           &
                    + cvara_r22(iel)                           &
                    + cvara_r33(iel) )                         &
                    / cvara_ep(iel)

    smbr(iel)   = smbr(iel) + ce4 *tseps *cvara_ep(iel) /kseps
    rovsdt(iel) = rovsdt(iel) + max( (-ce4*tseps/kseps) , zero)
  enddo

endif

!===============================================================================
! 4. Mass source term
!===============================================================================

if (ncesmp.gt.0) then

  !       Integer equal to 1 (forr navsto: nb of sur-iter)
  iiun = 1

  !       We incremente smbr with -Gamma rtpa and rovsdt with Gamma (*theta)
  call catsma &
  !==========
 ( ncelet , ncel   , ncesmp , iiun   , isto2t , thetv ,           &
   icetsm , itypsm(:,ivar)  ,                                     &
   volume , rtpa(:,ivar)    , smacel(:,ivar)   , smacel(:,ipr) ,  &
   smbr   , rovsdt , w1 )

  !       If we extrapolate the source terms, we put Gamma Pinj in propce
  if(isto2t.gt.0) then
    do iel = 1, ncel
      propce(iel,iptsta+isou-1) =                                 &
      propce(iel,iptsta+isou-1) + w1(iel)
    enddo
  !       Otherwise we put it directly in smbr
  else
    do iel = 1, ncel
      smbr(iel) = smbr(iel) + w1(iel)
    enddo
  endif

endif

!===============================================================================
! 5. Non-stationary term
!===============================================================================

do iel = 1, ncel
  rovsdt(iel) = rovsdt(iel)                                          &
              + istat(ivar)*(crom(iel)/dt(iel))*volume(iel)
enddo

!===============================================================================
! 6. Production (rho * Ce1 * epsilon / k * P)
!    Dissipation (rho*Ce2.epsilon/k*epsilon)
!===============================================================================

if (isto2t.gt.0) then
  thetap = thetv
else
  thetap = 1.d0
endif

! ---> Calculation the production trace, depending we are in standard
!     Rij or in SSG (use of produc or grdvit)
if (iturb.eq.30) then
  do iel = 1, ncel
    w9(iel) = 0.5d0*(produc(1,iel)+produc(2,iel)+produc(3,iel))
  enddo
else
  do iel = 1, ncel
    w9(iel) = -( cvara_r11(iel)*gradv(1, 1, iel) +               &
                 cvara_r12(iel)*gradv(2, 1, iel) +               &
                 cvara_r13(iel)*gradv(3, 1, iel) +               &
                 cvara_r12(iel)*gradv(1, 2, iel) +               &
                 cvara_r22(iel)*gradv(2, 2, iel) +               &
                 cvara_r23(iel)*gradv(3, 2, iel) +               &
                 cvara_r13(iel)*gradv(1, 3, iel) +               &
                 cvara_r23(iel)*gradv(2, 3, iel) +               &
                 cvara_r33(iel)*gradv(3, 3, iel) )
  enddo
endif


! EBRSM
if (iturb.eq.32) then

  do iel = 1, ncel
    ! Half-traces
    trprod = w9(iel)
    trrij  = 0.5d0 * (cvara_r11(iel) + cvara_r22(iel) + cvara_r33(iel))
    ! Calculation of the Durbin time scale
    xttke  = trrij/cvara_ep(iel)
    xttkmg = xct*sqrt(viscl(iel)/crom(iel)/cvara_ep(iel))
    xttdrb = max(xttke,xttkmg)

    prdeps = trprod/cvara_ep(iel)
    alpha3 = cvar_al(iel)**3

    ! Production (explicit)
    ! Compute of C_eps_1'
    w1(iel) = cromo(iel)*volume(iel)*                                 &
              ce1*(1.d0+xa1*(1.d0-alpha3)*prdeps)*trprod/xttdrb


    ! Dissipation (implicit)
    smbr(iel) = smbr(iel) - crom(iel)*volume(iel)*                    &
                             ceps2*cvara_ep(iel)/xttdrb

    rovsdt(iel) = rovsdt(iel)                                         &
                + ceps2*crom(iel)*volume(iel)*thetap/xttdrb
  enddo

! SSG and LRR
else

  do iel = 1, ncel
    ! Half-traces
    trprod = w9(iel)
    trrij  = 0.5d0 * (cvara_r11(iel) + cvara_r22(iel) + cvara_r33(iel))
    xttke  = trrij/cvara_ep(iel)
    ! Production (explicit)
    w1(iel) = cromo(iel)*volume(iel)*ce1/xttke*trprod

    ! Dissipation (implicit)
    smbr(iel) = smbr(iel)                                            &
              - crom(iel)*volume(iel)*ceps2*cvara_ep(iel)**2/trrij
    rovsdt(iel) = rovsdt(iel)                                        &
                + ceps2*crom(iel)*volume(iel)/xttke*thetap
  enddo

endif

! Extrapolation of source terms (2nd order in time)
if (isto2t.gt.0) then
  do iel = 1, ncel
    propce(iel,iptsta+isou-1) = propce(iel,iptsta+isou-1) + w1(iel)
  enddo
else
  do iel = 1, ncel
    smbr(iel) = smbr(iel) + w1(iel)
  enddo
endif

!===============================================================================
! 7. Buoyancy term
!===============================================================================

if (igrari.eq.1) then

  ! Allocate a work array
  allocate(w7(ncelet))

  do iel = 1, ncel
    w7(iel) = 0.d0
  enddo

  call rijthe(nscal, ivar, gradro, w7)
  !==========

  ! Extrapolation of source terms (2nd order in time)
  if (isto2t.gt.0) then
    do iel = 1, ncel
      propce(iel,iptsta+isou-1) = propce(iel,iptsta+isou-1) + w7(iel)
    enddo
  else
    do iel = 1, ncel
      smbr(iel) = smbr(iel) + w7(iel)
    enddo
  endif

  ! Free memory
  deallocate(w7)

endif

!===============================================================================
! 8. Diffusion term (Daly Harlow: generalized gradient hypothesis method)
!===============================================================================

! Symmetric tensor diffusivity (GGDH)
if (idften(ivar).eq.6) then

  call field_get_val_v(ivsten, visten)

  do iel = 1, ncel
    viscce(1,iel) = visten(1,iel)/sigmae + viscl(iel)
    viscce(2,iel) = visten(2,iel)/sigmae + viscl(iel)
    viscce(3,iel) = visten(3,iel)/sigmae + viscl(iel)
    viscce(4,iel) = visten(4,iel)/sigmae
    viscce(5,iel) = visten(5,iel)/sigmae
    viscce(6,iel) = visten(6,iel)/sigmae
  enddo

  iwarnp = iwarni(ivar)

  call vitens &
  !==========
 ( viscce , iwarnp ,             &
   weighf , weighb ,             &
   viscf  , viscb  )

! Scalar diffusivity
else

  do iel = 1, ncel
    w1(iel) = viscl(iel) + idifft(ivar)*visct(iel)/sigmae
  enddo

  call viscfa                    &
  !==========
 ( imvisf ,                      &
   w1     ,                      &
   viscf  , viscb  )

endif

!===============================================================================
! 9. Solving
!===============================================================================

if (isto2t.gt.0) then
  thetp1 = 1.d0 + thets
  do iel = 1, ncel
    smbr(iel) = smbr(iel) + thetp1*propce(iel,iptsta+isou-1)
  enddo
endif

iconvp = iconv (ivar)
idiffp = idiff (ivar)
ndircp = ndircl(ivar)
ireslp = iresol(ivar)
nitmap = nitmax(ivar)
nswrsp = nswrsm(ivar)
nswrgp = nswrgr(ivar)
imligp = imligr(ivar)
ircflp = ircflu(ivar)
ischcp = ischcv(ivar)
isstpp = isstpc(ivar)
iescap = 0
imucpp = 0
idftnp = idften(ivar)
iswdyp = iswdyn(ivar)
imgrp  = imgr  (ivar)
ncymxp = ncymax(ivar)
nitmfp = nitmgf(ivar)
iwarnp = iwarni(ivar)
blencp = blencv(ivar)
epsilp = epsilo(ivar)
epsrsp = epsrsm(ivar)
epsrgp = epsrgr(ivar)
climgp = climgr(ivar)
extrap = extrag(ivar)
relaxp = relaxv(ivar)
! all boundary convective flux with upwind
icvflb = 0

call codits &
!==========
 ( idtvar , ivar   , iconvp , idiffp , ireslp , ndircp , nitmap , &
   imrgra , nswrsp , nswrgp , imligp , ircflp ,                   &
   ischcp , isstpp , iescap , imucpp , idftnp , iswdyp ,          &
   imgrp  , ncymxp , nitmfp , ipp    , iwarnp ,                   &
   blencp , epsilp , epsrsp , epsrgp , climgp , extrap ,          &
   relaxp , thetv  ,                                              &
   rtpa(1,ivar)    , rtpa(1,ivar)    ,                            &
   coefap , coefbp , cofafp , cofbfp ,                            &
   imasfl , bmasfl ,                                              &
   viscf  , viscb  , viscce  , viscf  , viscb  , viscce  ,        &
   weighf , weighb ,                                              &
   icvflb , ivoid  ,                                              &
   rovsdt , smbr   , rtp(1,ivar)     , dpvar  ,                   &
   rvoid  , rvoid  )

! Free memory
deallocate(w1)
deallocate(w9)
deallocate(viscce)
deallocate(weighf, weighb)

!--------
! Formats
!--------

#if defined(_CS_LANG_FR)

 1000 format(/,'           Resolution de la variable ',A8,/)

#else

 1000 format(/,'           Solving variable ',A8           ,/)

#endif

!----
! End
!----

return

end subroutine
