/* This file is part of MyPaint.
 * Copyright (C) 2008 by Martin Renold <martinxyz@gmx.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY. See the COPYING file for more details.
 */

#define TILE_SIZE 64

class TiledSurface : public Surface {
  // the Python half of this class is in tiledsurface.py
private:
  PyObject * self;
  Rect dirty_bbox;
  int atomic;

  // caching tile memory location (optimization)
  #define TILE_MEMORY_SIZE 8
  typedef struct {
    int tx, ty;
    uint16_t * rgba_p;
  } TileMemory;
  TileMemory tileMemory[TILE_MEMORY_SIZE];
  int tileMemoryValid;
  int tileMemoryWrite;

public:
  TiledSurface(PyObject * self_) {
    self = self_; // no need to incref
    atomic = 0;
    dirty_bbox.w = 0;
    tileMemoryValid = 0;
    tileMemoryWrite = 0;
  }

  void begin_atomic() {
    if (atomic == 0) {
      assert(dirty_bbox.w == 0);
      assert(tileMemoryValid == 0);
    }
    atomic++;
  }
  void end_atomic() {
    assert(atomic > 0);
    atomic--;
    if (atomic == 0) {
      tileMemoryValid = 0;
      tileMemoryWrite = 0;
      Rect bbox = dirty_bbox; // copy to safety before calling python
      dirty_bbox.w = 0;
      if (bbox.w > 0) {
        PyObject* res;
        // OPTIMIZE: send a list tiles for minimal compositing? (but profile the code first)
        res = PyObject_CallMethod(self, "notify_observers", "(iiii)", bbox.x, bbox.y, bbox.w, bbox.h);
        if (!res) printf("Python exception during notify_observers! FIXME: Traceback will not be accurate.\n");
        Py_DECREF(res);
      }
    }
  }

  uint16_t * get_tile_memory(int tx, int ty, bool readonly) {
    // We assume that the memory location does not change between begin_atomic() and end_atomic().
    for (int i=0; i<tileMemoryValid; i++) {
      if (tileMemory[i].tx == tx and tileMemory[i].ty == ty) {
        return tileMemory[i].rgba_p;
      }
    }
    PyObject* rgba = PyObject_CallMethod(self, "get_tile_memory", "(iii)", tx, ty, readonly);
    if (rgba == NULL) {
      printf("Python exception during get_tile_memory()! The next traceback might be wrong.\n");
      return NULL;
    }
    /* time critical assertions
       assert(PyArray_NDIM(rgba) == 3);
       assert(PyArray_DIM(rgba, 0) == TILE_SIZE);
       assert(PyArray_DIM(rgba, 1) == TILE_SIZE);
       assert(PyArray_DIM(rgba, 2) == 4);
       assert(PyArray_ISCARRAY(rgba));
       assert(PyArray_TYPE(rgba) == NPY_UINT16);
    */
    // tiledsurface.py will keep a reference in its tiledict, at least until the final end_atomic()
    Py_DECREF(rgba);
    uint16_t * rgba_p = (uint16_t*)((PyArrayObject*)rgba)->data;

    // Cache tiles to speed up small brush strokes with lots of dabs, like charcoal.
    // Not caching readonly requests; they are alternated with write requests anyway.
    if (!readonly) {
      if (tileMemoryValid < TILE_MEMORY_SIZE) {
        tileMemoryValid++;
      }
      // We always overwrite the oldest cache entry.
      // We are mainly optimizing for strokes with radius smaller than one tile.
      tileMemory[tileMemoryWrite].tx = tx;
      tileMemory[tileMemoryWrite].ty = ty;
      tileMemory[tileMemoryWrite].rgba_p = rgba_p;
      tileMemoryWrite = (tileMemoryWrite + 1) % TILE_MEMORY_SIZE;
    }
    return rgba_p;
  }

  // returns true if the surface was modified
  bool draw_dab (float x, float y, 
                 float radius, 
                 float color_r, float color_g, float color_b,
                 float opaque, float hardness = 0.5,
                 float alpha_eraser = 1.0
                 ) {

    float r_fringe;
    int xp, yp;
    float xx, yy, rr;
    float one_over_radius2;

    assert(color_r >= 0.0 && color_r <= 1.0);
    assert(color_g >= 0.0 && color_g <= 1.0);
    assert(color_b >= 0.0 && color_b <= 1.0);
    uint32_t color_r_ = color_r * (1<<15);
    uint32_t color_g_ = color_g * (1<<15);
    uint32_t color_b_ = color_b * (1<<15);

    if (opaque == 0) return false;
    if (radius < 0.1) return false;
    if (hardness == 0) return false; // infintly small point, rest transparent

    assert(atomic > 0);

    r_fringe = radius + 1;
    rr = radius*radius;
    one_over_radius2 = 1.0/rr;

    int tx1 = floor(floor(x - r_fringe) / TILE_SIZE);
    int tx2 = floor(floor(x + r_fringe) / TILE_SIZE);
    int ty1 = floor(floor(y - r_fringe) / TILE_SIZE);
    int ty2 = floor(floor(y + r_fringe) / TILE_SIZE);
    int tx, ty;
    for (ty = ty1; ty <= ty2; ty++) {
      for (tx = tx1; tx <= tx2; tx++) {
        uint16_t * rgba_p = get_tile_memory(tx, ty, false);
        if (!rgba_p) return true; // python exception

        float xc = x - tx*TILE_SIZE;
        float yc = y - ty*TILE_SIZE;

        int x0 = floor (xc - r_fringe);
        int y0 = floor (yc - r_fringe);
        int x1 = ceil (xc + r_fringe);
        int y1 = ceil (yc + r_fringe);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > TILE_SIZE-1) x1 = TILE_SIZE-1;
        if (y1 > TILE_SIZE-1) y1 = TILE_SIZE-1;

        for (yp = y0; yp <= y1; yp++) {
          yy = (yp + 0.5 - yc);
          yy *= yy;
          for (xp = x0; xp <= x1; xp++) {
            xx = (xp + 0.5 - xc);
            xx *= xx;
            rr = (yy + xx) * one_over_radius2;
            // rr is in range 0.0..1.0*sqrt(2)

            if (rr <= 1.0) {
              float opa = opaque;
              if (hardness < 1.0) {
                if (rr < hardness) {
                  opa *= rr + 1-(rr/hardness);
                  // hardness == 0 is nonsense, excluded above
                } else {
                  opa *= hardness/(hardness-1)*(rr-1);
                }
              }

              // We are manipulating pixels with premultiplied alpha directly.
              // This is an "over" operation (opa = topAlpha).
              // In the formula below, topColor is assumed to be premultiplied.
              //
              //               opa_a      <   opa_b      >
              // resultAlpha = topAlpha + (1.0 - topAlpha) * bottomAlpha
              // resultColor = topColor + (1.0 - topAlpha) * bottomColor
              //
              // (at least for the normal case where alpha_eraser == 1.0)
              // OPTIMIZE: separate function for the standard case without erasing?
              // OPTIMIZE: don't use floats here in the inner loop?

              //assert(opa >= 0.0 && opa <= 1.0);
              //assert(alpha_eraser >= 0.0 && alpha_eraser <= 1.0);

              uint32_t opa_a = (1<<15)*opa;   // topAlpha
              uint32_t opa_b = (1<<15)-opa_a; // bottomAlpha
              
              // only for eraser, or for painting with translucent-making colors
              opa_a *= alpha_eraser;
              
              int idx = (yp*TILE_SIZE + xp)*4;
              rgba_p[idx+3] = opa_a + (opa_b*rgba_p[idx+3])/(1<<15);
              rgba_p[idx+0] = (opa_a*color_r_ + opa_b*rgba_p[idx+0])/(1<<15);
              rgba_p[idx+1] = (opa_a*color_g_ + opa_b*rgba_p[idx+1])/(1<<15);
              rgba_p[idx+2] = (opa_a*color_b_ + opa_b*rgba_p[idx+2])/(1<<15);
            }
          }
        }
      }
    }

    {
      // expand the bounding box to include the region we just drawed
      int bb_x, bb_y, bb_w, bb_h;
      bb_x = floor (x - (radius+1));
      bb_y = floor (y - (radius+1));
      /* FIXME: think about it exactly */
      bb_w = ceil (2*(radius+1));
      bb_h = ceil (2*(radius+1));

      ExpandRectToIncludePoint (&dirty_bbox, bb_x, bb_y);
      ExpandRectToIncludePoint (&dirty_bbox, bb_x+bb_w-1, bb_y+bb_h-1);
    }

    return true;
  }

  void get_color (float x, float y, 
                  float radius, 
                  float * color_r, float * color_g, float * color_b, float * color_a
                  ) {

    float r_fringe;
    int xp, yp;
    float xx, yy, rr;
    float one_over_radius2;

    assert(radius >= 0.1);
    const float hardness = 0.5;
    const float opaque = 1.0;

    float sum_r, sum_g, sum_b, sum_a, sum_weight;
    sum_r = sum_g = sum_b = sum_a = sum_weight = 0.0;

    // WARNING: some code duplication with draw_dab

    r_fringe = radius + 1;
    rr = radius*radius;
    one_over_radius2 = 1.0/rr;

    int tx1 = floor(floor(x - r_fringe) / TILE_SIZE);
    int tx2 = floor(floor(x + r_fringe) / TILE_SIZE);
    int ty1 = floor(floor(y - r_fringe) / TILE_SIZE);
    int ty2 = floor(floor(y + r_fringe) / TILE_SIZE);
    int tx, ty;
    for (ty = ty1; ty <= ty2; ty++) {
      for (tx = tx1; tx <= tx2; tx++) {
        uint16_t * rgba_p = get_tile_memory(tx, ty, true);
        if (!rgba_p) return; // python exception

        float xc = x - tx*TILE_SIZE;
        float yc = y - ty*TILE_SIZE;

        int x0 = floor (xc - r_fringe);
        int y0 = floor (yc - r_fringe);
        int x1 = ceil (xc + r_fringe);
        int y1 = ceil (yc + r_fringe);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > TILE_SIZE-1) x1 = TILE_SIZE-1;
        if (y1 > TILE_SIZE-1) y1 = TILE_SIZE-1;

        for (yp = y0; yp <= y1; yp++) {
          yy = (yp + 0.5 - yc);
          yy *= yy;
          for (xp = x0; xp <= x1; xp++) {
            xx = (xp + 0.5 - xc);
            xx *= xx;
            rr = (yy + xx) * one_over_radius2;
            // rr is in range 0.0..1.0*sqrt(2)

            if (rr <= 1.0) {
              float opa = opaque;
              if (hardness < 1.0) {
                if (rr < hardness) {
                  opa *= rr + 1-(rr/hardness);
                  // hardness == 0 is nonsense, excluded above
                } else {
                  opa *= hardness/(hardness-1)*(rr-1);
                }
              }

              // note that we are working on premultiplied alpha
              // we do not un-premultiply it yet, so colors are weighted with their alpha
              int idx = (yp*TILE_SIZE + xp)*4;
              sum_weight += opa;
              sum_r      += opa*rgba_p[idx+0]/(1<<15);
              sum_g      += opa*rgba_p[idx+1]/(1<<15);
              sum_b      += opa*rgba_p[idx+2]/(1<<15);
              sum_a      += opa*rgba_p[idx+3]/(1<<15);
            }
          }
        }
      }
    }

    assert(sum_weight > 0.0);
    sum_a /= sum_weight;
    sum_r /= sum_weight;
    sum_g /= sum_weight;
    sum_b /= sum_weight;

    *color_a = sum_a;
    // now un-premultiply the alpha
    if (sum_a > 0.0) {
      *color_r = sum_r / sum_a;
      *color_g = sum_g / sum_a;
      *color_b = sum_b / sum_a;
    } else {
      // it is all transparent, so don't care about the colors
      // (let's make them ugly so bugs will be visible)
      *color_r = 0.0;
      *color_g = 1.0;
      *color_b = 0.0;
    }
    assert (*color_a >= 0.0);
    assert (*color_r >= 0.0);
    assert (*color_g >= 0.0);
    assert (*color_b >= 0.0);
    assert (*color_r <= 1.001);
    assert (*color_g <= 1.001);
    assert (*color_b <= 1.001);
  }

  float get_alpha (float x, float y, float radius) {
    float color_r, color_g, color_b, color_a;
    get_color (x, y, radius, &color_r, &color_g, &color_b, &color_a);
    return color_a;
  }
};

void composite_tile_over_rgb8(PyObject * src, PyObject * dst) {
  assert(PyArray_DIM(src, 0) == TILE_SIZE);
  assert(PyArray_DIM(src, 1) == TILE_SIZE);
  assert(PyArray_DIM(src, 2) == 4);
  assert(PyArray_TYPE(src) == NPY_UINT16);
  assert(PyArray_ISCARRAY(src));

  assert(PyArray_DIM(dst, 0) == TILE_SIZE);
  assert(PyArray_DIM(dst, 1) == TILE_SIZE);
  assert(PyArray_DIM(dst, 2) == 3);
  assert(PyArray_TYPE(dst) == NPY_UINT8);
  assert(PyArray_ISBEHAVED(dst));
  
  PyArrayObject* dst_arr = ((PyArrayObject*)dst);
  assert(dst_arr->strides[1] == 3*sizeof(uint8_t));
  assert(dst_arr->strides[2] ==   sizeof(uint8_t));

  uint16_t * src_p  = (uint16_t*)((PyArrayObject*)src)->data;
  char * p = dst_arr->data;
  for (int y=0; y<TILE_SIZE; y++) {
    uint8_t  * dst_p  = (uint8_t*) (p);
    for (int x=0; x<TILE_SIZE; x++) {
      // resultAlpha = 1.0 (thus it does not matter if resultColor is premultiplied alpha or not)
      // resultColor = topColor + (1.0 - topAlpha) * bottomColor
      const uint32_t one_minus_topAlpha = (1<<15) - src_p[3];
      dst_p[0] = ((uint32_t)src_p[0]*255 + one_minus_topAlpha*dst_p[0]) / (1<<15);
      dst_p[1] = ((uint32_t)src_p[1]*255 + one_minus_topAlpha*dst_p[1]) / (1<<15);
      dst_p[2] = ((uint32_t)src_p[2]*255 + one_minus_topAlpha*dst_p[2]) / (1<<15);
      src_p += 4;
      dst_p += 3;
    }
    p += dst_arr->strides[0];
  }
}
