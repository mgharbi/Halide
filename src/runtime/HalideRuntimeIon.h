#ifndef HALIDE_HALIDERUNTIMEION_H
#define HALIDE_HALIDERUNTIMEION_H

#include "HalideRuntime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \file
 *  Routines specific to the Halide Ion runtime.
 */

extern const struct halide_device_interface *halide_ion_device_interface();

/** Set the underlying ion device handle for a buffer. The device
 * handle should be allocated using ion_alloc or similar and must
 * have an extent large enough to cover that specified by the buffer_t
 * extent fields. The dev field of the buffer_t must be NULL when this
 * routine is called. This call can fail due to running out of memory
 * or being passed an invalid device handle. The device and host
 * dirty bits are left unmodified. */
extern int halide_ion_wrap_device_handle(void *user_context, struct buffer_t *buf, uintptr_t device_handle);

/** Disconnect this buffer_t from the device handle it was previously
 * wrapped around. Should only be called for a buffer_t that
 * halide_ion_wrap_device_handle was previously called on. Frees any
 * storage associated with the binding of the buffer_t and the device
 * handle, but does not free the device handle. The previously
 * wrapped device handle is returned. . The dev field of the buffer_t
 * will be NULL on return.
 */
extern uintptr_t halide_ion_detach_device_handle(void *user_context, struct buffer_t *buf);

/** Return the underlying device handle for a buffer_t. This buffer
 *  must be valid on a Ion device, or not have any associated device
 *  memory. If there is no device memory (dev field is NULL), this
 *  returns 0.
 */
extern uintptr_t halide_ion_get_device_handle(void *user_context, struct buffer_t *buf);

#ifdef __cplusplus
} // End extern "C"
#endif

#endif // HALIDE_HALIDERUNTIMEION_H
