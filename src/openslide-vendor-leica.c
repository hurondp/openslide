/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

/*
 * LEICA (scn) BigTIFF support
 *
 * quickhash comes from _openslide_tiff_init_properties_and_hash
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <tiffio.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

static const char LEICA_XMLNS[] = "http://www.leica-microsystems.com/scn/2010/10/01";
static const char LEICA_ATTR_SIZE_X[] = "sizeX";
static const char LEICA_ATTR_SIZE_Y[] = "sizeY";
static const char LEICA_ATTR_OFFSET_X[] = "offsetX";
static const char LEICA_ATTR_OFFSET_Y[] = "offsetY";
static const char LEICA_ATTR_IFD[] = "ifd";
static const char LEICA_ATTR_Z_PLANE[] = "z";
static const char LEICA_VALUE_BRIGHTFIELD[] = "brightfield";

#define PARSE_INT_ATTRIBUTE_OR_FAIL(NODE, NAME, OUT)		\
  do {								\
    GError *tmp_err = NULL;					\
    OUT = _openslide_xml_parse_int_attr(NODE, NAME, &tmp_err);	\
    if (tmp_err)  {						\
      g_propagate_error(err, tmp_err);				\
      goto FAIL;						\
    }								\
  } while (0)

struct leica_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  double clicks_per_pixel;
  GPtrArray *areas;
};

// a TIFF directory within a level
struct area {
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;

  int64_t clicks_offset_x;
  int64_t clicks_offset_y;
};

struct read_tile_args {
  TIFF *tiff;
  struct area *area;
};

/* structs representing data parsed from ImageDescription XML */
struct collection {
  char *barcode;

  int64_t clicks_across;
  int64_t clicks_down;

  GPtrArray *images;
};

struct image {
  char *creation_date;
  char *device_model;
  char *device_version;
  char *illumination_source;

  // doubles, but not parsed
  char *objective;
  char *aperture;

  bool is_macro;
  int64_t clicks_across;
  int64_t clicks_down;
  int64_t clicks_offset_x;
  int64_t clicks_offset_y;

  GPtrArray *dimensions;
};

struct dimension {
  int64_t dir;
  int64_t width;
  int64_t height;
  double clicks_per_pixel;
};

static void destroy_level(struct level *l) {
  for (uint32_t n = 0; n < l->areas->len; n++) {
    struct area *area = l->areas->pdata[n];
    _openslide_grid_destroy(area->grid);
    g_slice_free(struct area, area);
  }
  g_slice_free(struct level, l);
}

static void destroy_data(struct leica_ops_data *data,
                         struct level **levels, int32_t level_count) {
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct leica_ops_data, data);

  for (int32_t i = 0; i < level_count; i++) {
    destroy_level(levels[i]);
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  struct leica_ops_data *data = osr->data;
  struct level **levels = (struct level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level G_GNUC_UNUSED,
                      int64_t tile_col, int64_t tile_row,
                      void *arg,
                      GError **err) {
  struct read_tile_args *args = arg;
  struct _openslide_tiff_level *tiffl = &args->area->tiffl;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            args->area, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    if (!_openslide_tiff_read_tile(tiffl, args->tiff,
                                   tiledata, tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, tiledata,
                                   tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // put it in the cache
    _openslide_cache_put(osr->cache,
			 args->area, tile_col, tile_row,
			 tiledata, tw * th * 4,
			 &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 tw, th,
                                                                 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 struct _openslide_level *level,
			 int32_t w, int32_t h,
			 GError **err) {
  struct leica_ops_data *data = osr->data;
  struct level *l = (struct level *) level;
  bool success = true;

  TIFF *tiff = _openslide_tiffcache_get(data->tc, err);
  if (tiff == NULL) {
    return false;
  }

  for (uint32_t n = 0; n < l->areas->len; n++) {
    struct area *area = l->areas->pdata[n];

    if (!TIFFSetDirectory(tiff, area->tiffl.dir)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot set TIFF directory");
      success = false;
      break;
    }

    struct read_tile_args args = {
      .tiff = tiff,
      .area = area,
    };
    int64_t ax = x / l->base.downsample -
                 area->clicks_offset_x / l->clicks_per_pixel;
    int64_t ay = y / l->base.downsample -
                 area->clicks_offset_y / l->clicks_per_pixel;
    success = _openslide_grid_paint_region(area->grid, cr, &args,
                                           ax, ay, level, w, h,
                                           err);
    if (!success) {
      break;
    }
  }

  _openslide_tiffcache_put(data->tc, tiff);
  return success;
}

static const struct _openslide_ops leica_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static void collection_free(struct collection *collection) {
  if (!collection) {
    return;
  }
  for (uint32_t image_num = 0; image_num < collection->images->len;
       image_num++) {
    struct image *image = collection->images->pdata[image_num];
    for (uint32_t dimension_num = 0; dimension_num < image->dimensions->len;
         dimension_num++) {
      struct dimension *dimension = image->dimensions->pdata[dimension_num];
      g_slice_free(struct dimension, dimension);
    }
    g_ptr_array_free(image->dimensions, true);
    g_free(image->creation_date);
    g_free(image->device_model);
    g_free(image->device_version);
    g_free(image->illumination_source);
    g_free(image->objective);
    g_free(image->aperture);
    g_slice_free(struct image, image);
  }
  g_ptr_array_free(collection->images, true);
  g_free(collection->barcode);
  g_slice_free(struct collection, collection);
}

static int dimension_compare(const void *a, const void *b) {
  const struct dimension *da = *(const struct dimension **) a;
  const struct dimension *db = *(const struct dimension **) b;

  if (da->width > db->width) {
    return -1;
  } else if (da->width == db->width) {
    return 0;
  } else {
    return 1;
  }
}

static void set_resolution_prop(openslide_t *osr, TIFF *tiff,
                                const char *property_name,
                                ttag_t tag) {
  float f;
  uint16_t unit;

  if (TIFFGetFieldDefaulted(tiff, TIFFTAG_RESOLUTIONUNIT, &unit) &&
      TIFFGetField(tiff, tag, &f) &&
      osr &&
      unit == RESUNIT_CENTIMETER) {
    g_hash_table_insert(osr->properties, g_strdup(property_name),
                        _openslide_format_double(10000.0 / f));
  }
}

static struct collection *parse_xml_description(const char *xml,
                                                GError **err) {
  xmlXPathContext *ctx = NULL;
  xmlXPathObject *images_result = NULL;
  xmlXPathObject *result = NULL;
  struct collection *collection = NULL;
  GError *tmp_err = NULL;
  bool success = false;

  // try to parse the xml
  xmlDoc *doc = _openslide_xml_parse(xml, &tmp_err);
  if (doc == NULL) {
    // not leica
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "%s", tmp_err->message);
    g_clear_error(&tmp_err);
    goto FAIL;
  }

  if (!_openslide_xml_has_default_namespace(doc, LEICA_XMLNS)) {
    // not leica
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Unexpected XML namespace");
    goto FAIL;
  }

  // create XPATH context to query the document
  ctx = _openslide_xml_xpath_create(doc);

  // the recognizable structure is the following:
  /*
    scn (root node)
      collection
        barcode
        image
          dimension
          dimension
        image
          dimension
          dimension
  */

  // get collection node
  xmlNode *collection_node = _openslide_xml_xpath_get_node(ctx,
                                                           "/d:scn/d:collection");
  if (!collection_node) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find collection element");
    goto FAIL;
  }

  // create collection struct
  collection = g_slice_new0(struct collection);
  collection->images = g_ptr_array_new();

  collection->barcode = _openslide_xml_xpath_get_string(ctx, "/d:scn/d:collection/d:barcode/text()");

  PARSE_INT_ATTRIBUTE_OR_FAIL(collection_node, LEICA_ATTR_SIZE_X,
                              collection->clicks_across);
  PARSE_INT_ATTRIBUTE_OR_FAIL(collection_node, LEICA_ATTR_SIZE_Y,
                              collection->clicks_down);

  // get the image nodes
  ctx->node = collection_node;
  images_result = _openslide_xml_xpath_eval(ctx, "d:image");
  if (!images_result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find any images");
    goto FAIL;
  }

  // create image structs
  for (int i = 0; i < images_result->nodesetval->nodeNr; i++) {
    xmlNode *image_node = images_result->nodesetval->nodeTab[i];
    ctx->node = image_node;

    // get view node
    xmlNode *view = _openslide_xml_xpath_get_node(ctx, "d:view");
    if (!view) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't find view node");
      goto FAIL;
    }

    // create image struct
    struct image *image = g_slice_new0(struct image);
    image->dimensions = g_ptr_array_new();

    image->creation_date = _openslide_xml_xpath_get_string(ctx, "d:creationDate/text()");
    image->device_model = _openslide_xml_xpath_get_string(ctx, "d:device/@model");
    image->device_version = _openslide_xml_xpath_get_string(ctx, "d:device/@version");
    image->illumination_source = _openslide_xml_xpath_get_string(ctx, "d:scanSettings/d:illuminationSettings/d:illuminationSource/text()");
    image->objective = _openslide_xml_xpath_get_string(ctx, "d:scanSettings/d:objectiveSettings/d:objective/text()");
    image->aperture = _openslide_xml_xpath_get_string(ctx, "d:scanSettings/d:illuminationSettings/d:numericalAperture/text()");

    PARSE_INT_ATTRIBUTE_OR_FAIL(view, LEICA_ATTR_SIZE_X,
                                image->clicks_across);
    PARSE_INT_ATTRIBUTE_OR_FAIL(view, LEICA_ATTR_SIZE_Y,
                                image->clicks_down);
    PARSE_INT_ATTRIBUTE_OR_FAIL(view, LEICA_ATTR_OFFSET_X,
                                image->clicks_offset_x);
    PARSE_INT_ATTRIBUTE_OR_FAIL(view, LEICA_ATTR_OFFSET_Y,
                                image->clicks_offset_y);

    image->is_macro = (image->clicks_offset_x == 0 &&
                       image->clicks_offset_y == 0 &&
                       image->clicks_across == collection->clicks_across &&
                       image->clicks_down == collection->clicks_down);

    // get dimensions
    ctx->node = image_node;
    result = _openslide_xml_xpath_eval(ctx, "d:pixels/d:dimension");
    if (!result) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't find any dimensions in image");
      goto FAIL;
    }

    // create dimension structs
    for (int i = 0; i < result->nodesetval->nodeNr; i++) {
      xmlNode *dimension_node = result->nodesetval->nodeTab[i];

      // accept only dimensions from z-plane 0
      // TODO: support multiple z-planes
      xmlChar *z = xmlGetProp(dimension_node, BAD_CAST LEICA_ATTR_Z_PLANE);
      if (z && strcmp((char *) z, "0")) {
        xmlFree(z);
        continue;
      }
      xmlFree(z);

      struct dimension *dimension = g_slice_new0(struct dimension);

      PARSE_INT_ATTRIBUTE_OR_FAIL(dimension_node, LEICA_ATTR_IFD,
                                  dimension->dir);
      PARSE_INT_ATTRIBUTE_OR_FAIL(dimension_node, LEICA_ATTR_SIZE_X,
                                  dimension->width);
      PARSE_INT_ATTRIBUTE_OR_FAIL(dimension_node, LEICA_ATTR_SIZE_Y,
                                  dimension->height);

      dimension->clicks_per_pixel =
        (double) image->clicks_across / dimension->width;

      g_ptr_array_add(image->dimensions, dimension);
    }
    xmlXPathFreeObject(result);
    result = NULL;

    // sort dimensions
    g_ptr_array_sort(image->dimensions, dimension_compare);

    // add image
    g_ptr_array_add(collection->images, image);
  }

  success = true;

FAIL:
  xmlXPathFreeObject(result);
  xmlXPathFreeObject(images_result);
  xmlXPathFreeContext(ctx);
  if (doc != NULL) {
    xmlFreeDoc(doc);
  }

  if (success) {
    return collection;
  } else {
    collection_free(collection);
    return NULL;
  }
}

static void set_prop(openslide_t *osr, const char *name, const char *value) {
  if (osr && value) {
    g_hash_table_insert(osr->properties,
                        g_strdup(name),
                        g_strdup(value));
  }
}

// For compatibility, slides with 0-1 macro images, 1 brightfield main image,
// and no other main images quickhash the smallest main image dimension
// in z-plane 0.
// All other slides quickhash the lowest-resolution brightfield macro image.
static bool should_use_legacy_quickhash(const struct collection *collection) {
  uint32_t brightfield_main_images = 0;
  uint32_t macro_images = 0;
  for (uint32_t image_num = 0; image_num < collection->images->len;
       image_num++) {
    struct image *image = collection->images->pdata[image_num];
    if (image->is_macro) {
      macro_images++;
    } else {
      if (!image->illumination_source ||
          strcmp(image->illumination_source, LEICA_VALUE_BRIGHTFIELD)) {
        return false;
      }
      brightfield_main_images++;
    }
  }
  return (brightfield_main_images == 1 && macro_images <= 1);
}

// parent must free levels on failure
static bool create_levels_from_collection(openslide_t *osr,
                                          struct _openslide_tiffcache *tc,
                                          TIFF *tiff,
                                          struct collection *collection,
                                          GPtrArray *levels,
                                          int64_t *quickhash_dir,
                                          GError **err) {
  *quickhash_dir = -1;

  // set barcode property
  set_prop(osr, "leica.barcode", collection->barcode);

  // determine quickhash mode
  bool legacy_quickhash = should_use_legacy_quickhash(collection);
  //g_debug("legacy quickhash %s", legacy_quickhash ? "true" : "false");

  // process main image
  struct image *first_main_image = NULL;
  for (uint32_t image_num = 0; image_num < collection->images->len;
       image_num++) {
    struct image *image = collection->images->pdata[image_num];

    if (image->is_macro) {
      continue;
    }

    // we only support brightfield
    if (!image->illumination_source ||
        strcmp(image->illumination_source, LEICA_VALUE_BRIGHTFIELD)) {
      continue;
    }

    if (!first_main_image) {
      // first main image
      first_main_image = image;

      // add some properties
      set_prop(osr, "leica.aperture", image->aperture);
      set_prop(osr, "leica.creation-date", image->creation_date);
      set_prop(osr, "leica.device-model", image->device_model);
      set_prop(osr, "leica.device-version", image->device_version);
      set_prop(osr, "leica.illumination-source", image->illumination_source);
      set_prop(osr, "leica.objective", image->objective);

      // copy objective to standard property
      if (osr) {
        _openslide_duplicate_int_prop(osr->properties, "leica.objective",
                                      OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
      }
    }

    // verify that it's safe to composite this main image with the others
    if (strcmp(image->illumination_source,
               first_main_image->illumination_source) ||
        strcmp(image->objective, first_main_image->objective) ||
        image->dimensions->len != first_main_image->dimensions->len) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Slides with dissimilar main images are not supported");
      return false;
    }

    // add all the IFDs to the level list
    for (uint32_t dimension_num = 0; dimension_num < image->dimensions->len;
         dimension_num++) {
      struct dimension *dimension = image->dimensions->pdata[dimension_num];

      struct level *l;
      if (image == first_main_image) {
        // no level yet; create it
        l = g_slice_new0(struct level);
        l->areas = g_ptr_array_new();
        l->clicks_per_pixel = dimension->clicks_per_pixel;
        g_ptr_array_add(levels, l);
      } else {
        // get level
        g_assert(dimension_num < levels->len);
        l = levels->pdata[dimension_num];

        // minimize click density
        l->clicks_per_pixel = MIN(l->clicks_per_pixel,
                                  dimension->clicks_per_pixel);

        // verify compatible resolution, with some tolerance for rounding
        struct dimension *first_image_dimension =
          first_main_image->dimensions->pdata[dimension_num];
        double resolution_similarity = 1 - fabs(dimension->clicks_per_pixel -
          first_image_dimension->clicks_per_pixel) /
          first_image_dimension->clicks_per_pixel;
        //g_debug("resolution similarity %g", resolution_similarity);
        if (resolution_similarity < 0.98) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "Inconsistent main image resolutions");
          return false;
        }
      }

      // create area
      struct area *area = g_slice_new0(struct area);
      struct _openslide_tiff_level *tiffl = &area->tiffl;
      g_ptr_array_add(l->areas, area);

      // select and examine TIFF directory
      if (!_openslide_tiff_level_init(tiff, dimension->dir,
                                      NULL, tiffl,
                                      err)) {
        return false;
      }

      // set area offset
      area->clicks_offset_x = image->clicks_offset_x;
      area->clicks_offset_y = image->clicks_offset_y;
      //g_debug("directory %"G_GINT64_FORMAT", clicks/pixel %g", dimension->dir, dimension->clicks_per_pixel);

      // verify that we can read this compression (hard fail if not)
      uint16_t compression;
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Can't read compression scheme");
        return false;
      }
      if (!TIFFIsCODECConfigured(compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unsupported TIFF compression: %u", compression);
        return false;
      }

      // create grid
      area->grid = _openslide_grid_create_simple(osr,
                                                 tiffl->tiles_across,
                                                 tiffl->tiles_down,
                                                 tiffl->tile_w,
                                                 tiffl->tile_h,
                                                 read_tile);
    }

    // set quickhash directory in legacy mode
    if (legacy_quickhash && image == first_main_image) {
      struct dimension *dimension =
        image->dimensions->pdata[image->dimensions->len - 1];
      *quickhash_dir = dimension->dir;
    }
  }

  if (!first_main_image) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find main image");
    return false;
  }

  // now we have minimized click densities; set level sizes
  for (uint32_t level_num = 0; level_num < levels->len; level_num++) {
    struct level *l = levels->pdata[level_num];
    l->base.w = ceil(collection->clicks_across / l->clicks_per_pixel);
    l->base.h = ceil(collection->clicks_down / l->clicks_per_pixel);
    //g_debug("level %d, clicks/pixel %g", level_num, l->clicks_per_pixel);
  }

  // process macro image
  bool have_macro_image = false;
  for (uint32_t image_num = 0; image_num < collection->images->len;
       image_num++) {
    struct image *image = collection->images->pdata[image_num];

    if (!image->is_macro) {
      continue;
    }

    // we only support brightfield
    if (!image->illumination_source ||
        strcmp(image->illumination_source, LEICA_VALUE_BRIGHTFIELD)) {
      continue;
    }

    if (have_macro_image) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Found multiple macro images");
      return false;
    }

    // add associated image with largest dimension
    struct dimension *dimension = image->dimensions->pdata[0];
    if (!_openslide_tiff_add_associated_image(osr, "macro", tc,
                                              dimension->dir, err)) {
      return false;
    }

    // use smallest macro dimension for quickhash
    if (!legacy_quickhash) {
      dimension = image->dimensions->pdata[image->dimensions->len - 1];
      *quickhash_dir = dimension->dir;
    }

    have_macro_image = true;
  }

  if (*quickhash_dir == -1) {
    // e.g., new-style quickhash but no macro image
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Couldn't locate TIFF directory for quickhash");
    return false;
  }

  //g_debug("quickhash directory %"G_GINT64_FORMAT, *quickhash_dir);
  return true;
}

static bool leica_open(openslide_t *osr,
                       struct _openslide_tiffcache *tc, TIFF *tiff,
                       struct _openslide_hash *quickhash1, GError **err) {
  GPtrArray *level_array = g_ptr_array_new();

  if (!TIFFIsTiled(tiff)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "TIFF is not tiled");
    goto FAIL;
  }

  // get the xml description
  char *image_desc;
  int tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc);

  // check if it containes the XML namespace string before we invoke
  // the parser
  if (!tiff_result || !strstr(image_desc, LEICA_XMLNS)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a Leica slide");
    goto FAIL;
  }

  // read XML
  struct collection *collection = parse_xml_description(image_desc, err);
  if (!collection) {
    goto FAIL;
  }

  // initialize and verify levels
  int64_t quickhash_dir;
  if (!create_levels_from_collection(osr, tc, tiff, collection,
                                     level_array, &quickhash_dir, err)) {
    collection_free(collection);
    goto FAIL;
  }
  collection_free(collection);

  // unwrap level array
  int32_t level_count = level_array->len;
  g_assert(level_count > 0);
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // allocate private data
  struct leica_ops_data *data = g_slice_new0(struct leica_ops_data);

  if (osr == NULL) {
    // free now and return
    _openslide_tiffcache_put(tc, tiff);
    data->tc = tc;
    destroy_data(data, levels, level_count);
    return true;
  }

  // set hash and properties
  struct area *property_area = levels[0]->areas->pdata[0];
  tdir_t property_dir = property_area->tiffl.dir;
  if (!_openslide_tiff_init_properties_and_hash(osr, tiff, quickhash1,
                                                quickhash_dir,
                                                property_dir,
                                                err)) {
    destroy_data(data, levels, level_count);
    return false;
  }

  // keep the XML document out of the properties
  // (in case pyramid level 0 is also directory 0)
  g_hash_table_remove(osr->properties, OPENSLIDE_PROPERTY_NAME_COMMENT);
  g_hash_table_remove(osr->properties, "tiff.ImageDescription");

  // set MPP properties
  if (!TIFFSetDirectory(tiff, property_dir)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read directory");
    destroy_data(data, levels, level_count);
    return false;
  }
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_X,
                      TIFFTAG_XRESOLUTION);
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_Y,
                      TIFFTAG_YRESOLUTION);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &leica_ops;

  // put TIFF handle and assume tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  return true;

FAIL:
  // free the level array
  if (level_array) {
    for (uint32_t n = 0; n < level_array->len; n++) {
      destroy_level(level_array->pdata[n]);
    }
    g_ptr_array_free(level_array, true);
  }

  return false;
}

const struct _openslide_format _openslide_format_leica = {
  .name = "leica",
  .vendor = "leica",
  .open_tiff = leica_open,
};
