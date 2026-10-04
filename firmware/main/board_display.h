#ifndef NEMOSSI_BOARD_DISPLAY_H
#define NEMOSSI_BOARD_DISPLAY_H

#include <stdbool.h>
#include "esp_err.h"
#include "face.h"

esp_err_t nm_display_init(void);
/* Returns immediately if the preceding asynchronous SPI frame is in flight.
 * Only the UI task may call this function. A successful skip is not a redraw.
 */
esp_err_t nm_display_try_present(const face_model_t *face, bool *submitted);

#endif
