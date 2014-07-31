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
! --------
!> \file cs_coal_thfieldconv2.f90
!> \brief Calculation of the particles temperature
!>        Function with the solid enthalpy and concentrations
!-------------------------------------------------------------------------------

!-------------------------------------------------------------------------------
! Arguments
!______________________________________________________________________________.
!   mode          name          role
!______________________________________________________________________________!
!> \param[in]     ncelet        number of extended (real + ghost) cells
!> \param[in]     ncel          number of cells
!> \param[in]     rtp           variables calculation in cell centers
!>                               (current instant)
!> \param[in,out] propce        physical properties at cell centers
!> \param[in,out] eh0           real work array
!> \param[in,out] eh1           real work array
!______________________________________________________________________________!

subroutine cs_coal_thfieldconv2 &
 ( ncelet , ncel   ,                                              &
   rtp    , propce )

!==============================================================================
! Module files
!==============================================================================

use paramx
use numvar
use optcal
use dimens, only: nvar
use cstphy
use cstnum
use entsor
use ppppar
use ppthch
use coincl
use cpincl
use ppincl

!===============================================================================

implicit none

! Arguments

integer          ncelet, ncel
double precision rtp(ncelet,nflown:nvar), propce(ncelet,*)

! Local variables

integer          i      , icla   , icha   , icel
integer          ipcte1 , ipcte2
integer          ihflt2
double precision h2     , x2     , xch    , xck
double precision xash   , xnp    , xtes   , xwat

integer          iok1
double precision , dimension ( : )     , allocatable :: eh0,eh1

!===============================================================================
! Conversion method
!
ihflt2 = 1
!
!===============================================================================
! 1. Preliminary calculations
!===============================================================================

!===============================================================================
! Deallocation dynamic arrays
!----
allocate(eh0(1:ncel),eh1(1:ncel),STAT=iok1)
!----
if ( iok1 > 0 ) THEN
  write(nfecra,*) ' Memory allocation error inside: '
  write(nfecra,*) '    cs_coal_thfieldconv2         '
  call csexit(1)
endif
!===============================================================================

! --- Tables initialization
eh0( : ) = zero
eh1( : ) = zero

! --- Initialization from T2 to T1

ipcte1 = ipproc(itemp1)
do icla = 1, nclacp
  ipcte2 = ipproc(itemp2(icla))
  do icel = 1, ncel
    propce(icel,ipcte2) = propce(icel,ipcte1)
  enddo
enddo

!===============================================================================
! 2. Particles temperature calculation
!===============================================================================

if ( ihflt2.eq.0 ) then

! --> H2 linear function of T2

  do icla = 1, nclacp
    ipcte2 = ipproc(itemp2(icla))
    icha = ichcor(icla)
    do icel = 1, ncel
      propce(icel,ipcte2) =                                       &
            (rtp(icel,isca(ih2(icla)))-h02ch(icha))               &
            / cp2ch(icha) + trefth
    enddo
  enddo

else

! --> H2 tabule

  do icla = 1, nclacp

    ipcte2 = ipproc(itemp2(icla))

    i = npoc-1
    do icel = 1, ncel
      xch  = rtp(icel,isca(ixch(icla)))
      xnp  = rtp(icel,isca(inp(icla)))
      xck  = rtp(icel,isca(ixck(icla)))
      xash = xmash(icla)*xnp
      if ( ippmod(iccoal) .eq. 1 ) then
        xwat = rtp(icel,isca(ixwt(icla)))
      else
        xwat = 0.d0
      endif

      x2   = xch + xck + xash + xwat

      if ( x2 .gt. epsicp*100.d0 ) then

        h2   = rtp(icel,isca(ih2(icla)))/x2

        xtes = xmp0(icla)*xnp

        if ( xtes.gt.epsicp .and. x2.gt.epsicp*100.d0 ) then
          eh1(icel) = xch /x2 * ehsoli(ich(ichcor(icla) ),i+1)    &
                    + xck /x2 * ehsoli(ick(ichcor(icla) ),i+1)    &
                    + xash/x2 * ehsoli(iash(ichcor(icla)),i+1)    &
                    + xwat/x2 * ehsoli(iwat(ichcor(icla)),i+1)
          if ( h2.ge.eh1(icel) ) then
            propce(icel,ipcte2) = thc(i+1)

          endif
        endif

      endif

    enddo

    i = 1
    do icel = 1, ncel
      xch  = rtp(icel,isca(ixch(icla)))
      xnp  = rtp(icel,isca(inp(icla)))
      xck  = rtp(icel,isca(ixck(icla)))
      xash = xmash(icla)*xnp
      if ( ippmod(iccoal) .eq. 1 ) then
        xwat = rtp(icel,isca(ixwt(icla)))
      else
        xwat = 0.d0
      endif

      x2   = xch + xck + xash + xwat

      if ( x2 .gt. epsicp*100.d0 ) then

        h2   = rtp(icel,isca(ih2(icla)))/x2

        xtes = xmp0(icla)*xnp

        if ( xtes.gt.epsicp .and. x2.gt.epsicp*100.d0 ) then
          eh0(icel) = xch /x2 * ehsoli(ich(ichcor(icla) ),i)      &
                    + xck /x2 * ehsoli(ick(ichcor(icla) ),i)      &
                    + xash/x2 * ehsoli(iash(ichcor(icla)),i)      &
                    + xwat/x2 * ehsoli(iwat(ichcor(icla)),i)
          if ( h2.le.eh0(icel) ) then
            propce(icel,ipcte2) = thc(i)
          endif
        endif

      endif

    enddo

    do i = 1, npoc-1
      do icel = 1, ncel
        xch  = rtp(icel,isca(ixch(icla)))
        xnp  = rtp(icel,isca(inp(icla)))
        xck  = rtp(icel,isca(ixck(icla)))
        xash = xmash(icla)*xnp
        if ( ippmod(iccoal) .eq. 1 ) then
          xwat = rtp(icel,isca(ixwt(icla)))
        else
          xwat = 0.d0
        endif

        x2   = xch + xck + xash + xwat

        if ( x2 .gt. epsicp*100.d0 ) then

          h2   = rtp(icel,isca(ih2(icla)))/x2

          xtes = xmp0(icla)*xnp

          if ( xtes.gt.epsicp .and. x2.gt.epsicp*100.d0 ) then
            eh0(icel) = xch /x2 * ehsoli(ich(ichcor(icla) ),i  )  &
                      + xck /x2 * ehsoli(ick(ichcor(icla) ),i  )  &
                      + xash/x2 * ehsoli(iash(ichcor(icla)),i  )  &
                      + xwat/x2 * ehsoli(iwat(ichcor(icla)),i  )

            eh1(icel) = xch /x2 * ehsoli(ich(ichcor(icla) ),i+1)  &
                      + xck /x2 * ehsoli(ick(ichcor(icla) ),i+1)  &
                      + xash/x2 * ehsoli(iash(ichcor(icla)),i+1)  &
                      + xwat/x2 * ehsoli(iwat(ichcor(icla)),i+1)

            if ( h2.ge.eh0(icel) .and. h2.le.eh1(icel) ) then
              propce(icel,ipcte2) = thc(i) + (h2-eh0(icel)) *     &
                    (thc(i+1)-thc(i))/(eh1(icel)-eh0(icel))
            endif
          endif

        endif

      enddo
    enddo

  enddo

endif

!===============================================================================
! Deallocation dynamic arrays
!----
deallocate(eh0,eh1,STAT=iok1)
!----
if ( iok1 > 0 ) then
  write(nfecra,*) ' Memory deallocation error inside: '
  write(nfecra,*) '    cs_coal_thfieldconv2           '
  call csexit(1)
endif
!===============================================================================

!----
! End
!----

return
end subroutine
