/**
  Bring-up stage beacons.

  The beacons used to paint a colored square per startup stage into the
  bottom strip of the panel, straight into the LK framebuffer. They are
  disabled for the shipping UI; call sites stay so they can be re-enabled
  by restoring the drawing here.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef STAGE_BEACON_H_
#define STAGE_BEACON_H_

STATIC inline VOID
StageBeacon (
  IN UINTN  Index
  )
{
  (VOID)Index;
}

#endif /* STAGE_BEACON_H_ */
