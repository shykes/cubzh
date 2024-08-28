// -------------------------------------------------------------
//  Cubzh Core
//  serialization_v6.c
//  Created by Adrien Duermael on July 25, 2019.
// -------------------------------------------------------------

#include "serialization_v6.h"

#include <stdlib.h>
#include <string.h>

#include "cclog.h"
#include "map_string_float3.h"
#include "serialization.h"
#include "stream.h"
#include "transform.h"
#include "zlib.h"

typedef enum P3sCompressionMethod {
    P3sCompressionMethod_NONE = 0,
    P3sCompressionMethod_ZIP = 1,
    P3sCompressionMethod_COUNT = 2,
} P3sCompressionMethod;

#define P3S_CHUNK_ID_NONE 0 // not used as a chunk ID
#define P3S_CHUNK_ID_PREVIEW 1
#define P3S_CHUNK_ID_PALETTE_LEGACY 2
#define P3S_CHUNK_ID_SHAPE 3
#define P3S_CHUNK_ID_SHAPE_SIZE 4 // size of the shape (boundaries)
#define P3S_CHUNK_ID_SHAPE_BLOCKS 5
#define P3S_CHUNK_ID_SHAPE_POINT 6
#define P3S_CHUNK_ID_SHAPE_BAKED_LIGHTING 7
#define P3S_CHUNK_ID_SHAPE_POINT_ROTATION 8
// #define P3S_CHUNK_ID_SELECTED_COLOR 8
// #define P3S_CHUNK_ID_SELECTED_BACKGROUND_COLOR 9
// #define P3S_CHUNK_ID_CAMERA 10
// #define P3S_CHUNK_ID_DIRECTIONAL_LIGHT 11
// #define P3S_CHUNK_ID_SOURCE_METADATA 12
// #define P3S_CHUNK_ID_GENERAL_RENDERING_OPTIONS 14
#define P3S_CHUNK_ID_PALETTE_ID 15
#define P3S_CHUNK_ID_PALETTE 16
#define P3S_CHUNK_ID_SHAPE_ID 17        // id used to parent objects and will be used for animations
#define P3S_CHUNK_ID_SHAPE_NAME 18      // lenName, name (optional)
#define P3S_CHUNK_ID_SHAPE_PARENT_ID 19 // ID of parent
#define P3S_CHUNK_ID_SHAPE_TRANSFORM                                                               \
    20 // transform (position,rotation,scale) (optional, default 0,0,0, 0,0,0 and 1,1,1)
#define P3S_CHUNK_ID_SHAPE_PIVOT 21          // pivot
#define P3S_CHUNK_ID_SHAPE_PALETTE 22        // palette
#define P3S_CHUNK_ID_OBJECT_COLLISION_BOX 23 // collision box
#define P3S_CHUNK_ID_OBJECT_IS_HIDDEN 24     // isHidden
#define P3S_CHUNK_ID_MAX 25                  // /!\ update this when adding chunks

// size of the chunk header, without chunk ID (it's already read at this point)
#define CHUNK_V6_HEADER_NO_ID_SIZE (sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t))
#define CHUNK_V6_HEADER_NO_ID_SKIP_SIZE (sizeof(uint8_t) + sizeof(uint32_t))

// takes the 4 low bits of a and casts into uint8_t
#define TO_UINT4(a) (uint8_t)((a) & 0x0F)

// MARK: - Private functions prototypes -
// MARK: Write as buffer -

bool chunk_v6_shape_create_and_write_uncompressed_buffer(const Shape *shape,
                                                         uint16_t shapeId,
                                                         uint16_t shapeParentId,
                                                         const ColorPalette *sharedPalette,
                                                         uint32_t *uncompressedSize,
                                                         void **uncompressedData);

bool chunk_v6_shape_create_and_write_compressed_buffer(const Shape *shape,
                                                       uint16_t shapeId,
                                                       uint16_t shapeParentId,
                                                       const ColorPalette *sharedPalette,
                                                       uint32_t *uncompressedSize,
                                                       uint32_t *compressedSize,
                                                       void **compressedData);

void _chunk_v6_palette_create_and_write_uncompressed_buffer(
    const ColorPalette *palette,
    uint32_t *uncompressedSize,
    void **uncompressedData,
    SHAPE_COLOR_INDEX_INT_T **paletteMapping);

bool _chunk_v6_palette_create_and_write_compressed_buffer(const ColorPalette *palette,
                                                          uint32_t *uncompressedSize,
                                                          uint32_t *compressedSize,
                                                          void **compressedData,
                                                          SHAPE_COLOR_INDEX_INT_T **paletteMapping);

/// Writes chunk header and data
static bool write_chunk_in_buffer(void *destBuffer,
                                  uint8_t chunkID,
                                  bool isCompressed,
                                  const void *chunkWriteData,
                                  uint32_t chunkCompressedDataSize,
                                  uint32_t chunkUncompressedDataSize,
                                  uint32_t *externCursor);

/// Return value: true on success, false otherwise.
static bool write_preview_chunk_in_buffer(void *destBuffer,
                                          const void *previewBytes,
                                          const uint32_t previewBytesCount,
                                          uint32_t *externCursor);

// MARK: Write as file -

bool v6_write_size_at(long position, uint32_t size, FILE *fd);
// Writes full chunk (header + data) to file, compress the data if required, function will free data
// when done
bool chunk_v6_write_file(uint8_t chunkID, uint32_t size, void *data, uint8_t doCompress, FILE *fd);
bool chunk_v6_write_shape(FILE *fd,
                          Shape *shape,
                          uint16_t *shapeId,
                          uint16_t shapeParentId,
                          const ColorPalette *sharedPalette,
                          bool doCompress);
bool chunk_v6_write_preview_image(FILE *fd, const void *imageData, uint32_t imageDataSize);

// MARK: Read -

// all read functions return number of bytes read or 0 if the file can't be read
uint8_t chunk_v6_read_identifier(Stream *s);
uint32_t chunk_v6_read_size(Stream *s);
// Reads full chunk, uncompressing it if necessary,
// function allocates data that must be freed by caller
bool chunk_v6_read(void **chunkData, uint32_t *chunkSize, uint32_t *uncompressedSize, Stream *s);

// TODO: unify headers, currently only chunks writing with the function chunk_v6_write_file use v6
// header ie. Shape & Palette skips a chunk with v5 header (only chunkSize as uint32_t)
uint32_t chunk_v6_with_v5_header_skip(Stream *s);

// skips a chunk with v6 header
uint32_t chunk_v6_skip(Stream *s);

uint32_t chunk_v6_read_palette(Stream *s,
                               ColorAtlas *colorAtlas,
                               ColorPalette **palette,
                               bool isLegacy);

uint32_t chunk_v6_read_palette_id(Stream *s, uint8_t *paletteID);

// @param shrinkPalette used as reference to build a shrinked palette w/ only used colors
uint32_t chunk_v6_read_shape_process_blocks(void *cursor,
                                            Shape *shape,
                                            uint16_t w,
                                            uint16_t h,
                                            uint16_t d,
                                            uint8_t paletteID,
                                            ColorPalette *shrinkPalette);

// chunk_v6_read_shape allocates a new Shape if shape != NULL
uint32_t chunk_v6_read_shape(Stream *s,
                             Shape **shape,
                             DoublyLinkedList *shapes,
                             const LoadShapeSettings *const shapeSettings,
                             ColorAtlas *colorAtlas,
                             ColorPalette *filePalette,
                             uint8_t paletteID,
                             ColorPalette **rootShapePalette);

uint32_t chunk_v6_read_preview_image(Stream *s, void **imageData, uint32_t *size);

//  MARK: Utils -

static uint32_t getChunkHeaderSize(const uint8_t chunkID);
static uint32_t compute_preview_chunk_size(const uint32_t previewBytesCount);
static uint32_t compute_shape_chunk_size(uint32_t shapeBufferDataSize);

typedef struct _ShapeBuffers {
    uint32_t shapeUncompressedDataSize;
    uint32_t shapeCompressedDataSize;
    void *shapeCompressedData;
} ShapeBuffers;

static bool create_shape_buffers(DoublyLinkedList *shapeBuffers,
                                 Shape const *shape,
                                 uint16_t *shapeId,
                                 uint16_t shapeParentId,
                                 const ColorPalette *sharedPalette,
                                 uint32_t *size);

// MARK: - Exposed functions -

bool serialization_v6_save_shape(Shape *shape,
                                 const void *imageData,
                                 uint32_t imageDataSize,
                                 FILE *fd) {

    // -------------------
    // HEADER
    // -------------------

    // write file format version
    uint32_t format = 6;
    if (fwrite(&format, sizeof(uint32_t), 1, fd) != 1) {
        cclog_error("failed to write file format");
        return false;
    }

    // write compression algo
    const P3sCompressionMethod compressionAlgo = P3sCompressionMethod_ZIP;
    if (fwrite(&compressionAlgo, sizeof(uint8_t), 1, fd) != 1) {
        cclog_error("failed to write compression algo");
        return false;
    }

    long positionBeforeTotalSize = ftell(fd);

    // write total size (will be updated at the end)
    uint32_t totalSize = 0;
    if (fwrite(&totalSize, sizeof(uint32_t), 1, fd) != 1) {
        cclog_error("failed to write total size");
        return false;
    }

    long positionBeforeChunks = ftell(fd);

    // -------------------
    // CHUNKS
    // -------------------

    chunk_v6_write_preview_image(fd, imageData, imageDataSize);

    uint16_t shapeId = 1;
    chunk_v6_write_shape(fd, shape, &shapeId, 0, shape_get_palette(shape), true);

    // -------------------
    // END OF FILE
    // -------------------

    // update total size
    totalSize = (uint32_t)(ftell(fd) - positionBeforeChunks);

    if (v6_write_size_at(positionBeforeTotalSize, (uint32_t)totalSize, fd) == false) {
        cclog_error("failed to write compressed file size");
        return false;
    }

    return true;
}

/// serialize a shape in a newly created memory buffer
/// Arguments:
/// - shape (mandatory)
/// - palette (optional)
/// - imageData (optional)
bool serialization_v6_save_shape_as_buffer(const Shape *const shape,
                                           const ColorPalette *const artistPalette,
                                           const void *const previewData,
                                           const uint32_t previewDataSize,
                                           void **const outBuffer,
                                           uint32_t *const outBufferSize) {

    if (shape == NULL || outBuffer == NULL || outBufferSize == NULL) {
        return false;
    }

    const bool hasPreview = previewData != NULL && previewDataSize > 0;

    // --------------------------------------------------
    // Compute buffer size
    // --------------------------------------------------

    // Header
    uint32_t size = (MAGIC_BYTES_SIZE + SERIALIZATION_FILE_FORMAT_VERSION_SIZE +
                     SERIALIZATION_COMPRESSION_ALGO_SIZE + SERIALIZATION_TOTAL_SIZE_SIZE);

    // Preview
    if (hasPreview) {
        const uint32_t chunkSize = compute_preview_chunk_size(previewDataSize);
        size += chunkSize;
    }

    DoublyLinkedList *const shapesBuffers = doubly_linked_list_new();
    if (shapesBuffers == NULL) {
        return false;
    }

    uint16_t shapeId = 1;
    if (create_shape_buffers(shapesBuffers, shape, &shapeId, 0, shape_get_palette(shape), &size) ==
        false) {
        doubly_linked_list_free(shapesBuffers);
        return false;
    }

    bool hasArtistPalette = false;
    uint32_t paletteUncompressedDataSize = 0;
    uint32_t paletteCompressedDataSize = 0;
    void *paletteCompressedData = NULL;
    if (artistPalette != NULL) {
        SHAPE_COLOR_INDEX_INT_T *fakePaletteMapping;
        if (_chunk_v6_palette_create_and_write_compressed_buffer(artistPalette,
                                                                 &paletteUncompressedDataSize,
                                                                 &paletteCompressedDataSize,
                                                                 &paletteCompressedData,
                                                                 &fakePaletteMapping) == false) {
            return false;
        }
        size += getChunkHeaderSize(P3S_CHUNK_ID_PALETTE) + paletteCompressedDataSize;
        hasArtistPalette = true;
    }

    // allocate buffer
    uint8_t *buf = (uint8_t *)malloc(sizeof(uint8_t) * size);
    if (buf == NULL) {
        free(paletteCompressedData);
        doubly_linked_list_free(shapesBuffers);
        return false;
    }

    // writing cursor
    uint32_t cursor = 0;
    bool ok = false;

    // write magic bytes
    serialization_utils_writeCString(buf + cursor, MAGIC_BYTES, MAGIC_BYTES_SIZE, &cursor);

    // write file format version
    const uint32_t formatVersion = 6;
    serialization_utils_writeUint32(buf + cursor, formatVersion, &cursor);

    // write compression algo
    const P3sCompressionMethod compressionAlgo = P3sCompressionMethod_ZIP;
    serialization_utils_writeUint8(buf + cursor, compressionAlgo, &cursor);

    const uint32_t positionBeforeTotalSize = cursor;

    // write total size (will be updated at the end)
    uint32_t totalSize = 0;
    serialization_utils_writeUint32(buf + cursor, totalSize, &cursor);

    uint32_t positionBeforeChunks = cursor;

    // write preview
    if (hasPreview) {
        ok = write_preview_chunk_in_buffer(buf + cursor, previewData, previewDataSize, &cursor);
        if (ok == false) {
            free(buf);
            doubly_linked_list_free(shapesBuffers);
            return false;
        }
    }

    // write artist palette
    if (hasArtistPalette) {
        ok = write_chunk_in_buffer(buf + cursor,
                                   P3S_CHUNK_ID_PALETTE,
                                   true,
                                   paletteCompressedData,
                                   paletteCompressedDataSize,
                                   paletteUncompressedDataSize,
                                   &cursor);
        free(paletteCompressedData);
        if (ok == false) {
            free(buf);
            doubly_linked_list_free(shapesBuffers);
            return false;
        }
    }

    DoublyLinkedListNode *n = doubly_linked_list_first(shapesBuffers);
    ShapeBuffers *shapeBuffersCursor = NULL;
    while (n != NULL) {
        shapeBuffersCursor = (ShapeBuffers *)doubly_linked_list_node_pointer(n);

        ok = write_chunk_in_buffer(buf + cursor,
                                   P3S_CHUNK_ID_SHAPE,
                                   true,
                                   shapeBuffersCursor->shapeCompressedData,
                                   shapeBuffersCursor->shapeCompressedDataSize,
                                   shapeBuffersCursor->shapeUncompressedDataSize,
                                   &cursor);
        if (ok == false) {
            free(buf);
            doubly_linked_list_free(shapesBuffers);
            return false;
        }

        free(shapeBuffersCursor);
        n = doubly_linked_list_node_next(n);
    }

    doubly_linked_list_free(shapesBuffers);

    // update total size
    totalSize = cursor - positionBeforeChunks;

    memcpy(&buf[positionBeforeTotalSize], &totalSize, sizeof(uint32_t));

    *outBuffer = buf;
    *outBufferSize = cursor;

    return true;
}

/// get preview data from save file path (caller must free *imageData)
bool serialization_v6_get_preview_data(Stream *s, void **imageData, uint32_t *size) {

    uint8_t i;
    if (stream_read_uint8(s, &i) == false) {
        cclog_error("failed to read compression algo");
        return false;
    }
    P3sCompressionMethod compressionAlgo = (P3sCompressionMethod)i;

    // File header may mention a compression algorithm but the preview
    // chunk is never compressed (as it is already compressed, being a PNG)
    if (compressionAlgo >= P3sCompressionMethod_COUNT) {
        cclog_error("compression algo not supported (v6)");
        return false;
    }

    uint32_t totalSize = 0;

    if (stream_read_uint32(s, &totalSize) == false) {
        cclog_error("failed to read total size");
        return false;
    }

    // READ ALL CHUNKS UNTIL PREVIEW IMAGE IS FOUND

    uint32_t totalSizeRead = 0;
    uint32_t sizeRead = 0;

    uint8_t chunkID;

    while (totalSizeRead < totalSize) {
        chunkID = chunk_v6_read_identifier(s);
        totalSizeRead += 1; // size of chunk id

        switch (chunkID) {
            case P3S_CHUNK_ID_NONE:
                cclog_error("wrong chunk id found");
                return false;
            case P3S_CHUNK_ID_PREVIEW:
                sizeRead = chunk_v6_read_preview_image(s, imageData, size);
                if (sizeRead == 0) {
                    cclog_error("error while reading overview image");
                    return false;
                }
                return true;
            case P3S_CHUNK_ID_SHAPE:
            case P3S_CHUNK_ID_PALETTE:
            case P3S_CHUNK_ID_PALETTE_LEGACY:
            case P3S_CHUNK_ID_PALETTE_ID:
                // v6 chunks we don't need to read
                totalSizeRead += chunk_v6_skip(s);
                break;
            default:
                // v5 chunks we don't need to read
                totalSizeRead += chunk_v6_with_v5_header_skip(s);
                break;
        }
    }
    return false;
}

// MARK: - Private functions -

bool v6_write_size_at(long position, uint32_t size, FILE *fd) {

    long currentPosition = ftell(fd);

    fseek(fd, position, SEEK_SET);
    if (fwrite(&size, sizeof(uint32_t), 1, fd) != 1) {
        return false;
    }

    fseek(fd, currentPosition, SEEK_SET); // back to current position
    return true;
}

bool chunk_v6_write_file(uint8_t chunkID, uint32_t size, void *data, uint8_t doCompress, FILE *fd) {
    uint32_t chunkSize = size;
    const uint32_t uncompressedSize = size;

    // compress data if required by this chunk
    if (doCompress != 0) {
        uLong compressedSize = compressBound(size);
        void *compressedData = malloc(compressedSize);
        if (compress(compressedData, &compressedSize, data, size) != Z_OK) {
            // failed to compress data, let's free the destination buffer
            free(compressedData);
            free(data);
            return false;
        }
        free(data);
        chunkSize = (uint32_t)compressedSize;
        data = compressedData;
    }

    // write header
    if (fwrite(&chunkID, sizeof(uint8_t), 1, fd) != 1) {
        free(data);
        return false;
    }
    if (fwrite(&chunkSize, sizeof(uint32_t), 1, fd) != 1) {
        free(data);
        return false;
    }
    if (fwrite(&doCompress, sizeof(uint8_t), 1, fd) != 1) {
        free(data);
        return false;
    }
    if (fwrite(&uncompressedSize, sizeof(uint32_t), 1, fd) != 1) {
        free(data);
        return false;
    }
    // write data
    if (fwrite(data, chunkSize, 1, fd) != 1) {
        free(data);
        return false;
    }

    free(data);
    return true;
}

bool chunk_v6_write_shape(FILE *fd,
                          Shape *shape,
                          uint16_t *shapeId,
                          uint16_t shapeParentId,
                          const ColorPalette *sharedPalette,
                          bool doCompress) {

    if (fd == NULL) {
        return false;
    }
    if (shape == NULL) {
        return false;
    }

    uint32_t uncompressedSize = 0;
    void *uncompressedData = NULL;

    if (chunk_v6_shape_create_and_write_uncompressed_buffer(shape,
                                                            *shapeId,
                                                            shapeParentId,
                                                            sharedPalette,
                                                            &uncompressedSize,
                                                            &uncompressedData) == false) {
        cclog_error("chunk_v6_shape_create_and_write_uncompressed_buffer failed");
        return false;
    }

    /// write file
    // uncompressedData freed within chunk_v6_write_file
    if (chunk_v6_write_file(P3S_CHUNK_ID_SHAPE,
                            uncompressedSize,
                            uncompressedData,
                            doCompress,
                            fd) == false) {
        cclog_error("failed to write shape chunk");
        return false;
    }

    shapeParentId = *shapeId;
    (*shapeId)++;

    DoublyLinkedListNode *n = transform_get_children_iterator(shape_get_root_transform(shape));
    Transform *child = NULL;

    while (n != NULL) {
        child = (Transform *)(doubly_linked_list_node_pointer(n));
        // hide transforms reserved for engine
        Shape *childShape = transform_utils_get_shape(child);
        if (childShape != NULL) {
            chunk_v6_write_shape(fd, childShape, shapeId, shapeParentId, sharedPalette, true);
        }
        n = doubly_linked_list_node_next(n);
    }

    return true;
}

bool chunk_v6_write_preview_image(FILE *fd, const void *imageData, uint32_t imageDataSize) {
    const uint8_t chunkID = P3S_CHUNK_ID_PREVIEW;
    const uint32_t chunkSize = imageDataSize;

    // v5 chunk header
    if (fwrite(&chunkID, sizeof(uint8_t), 1, fd) != 1) {
        cclog_error("failed to write preview chunk ID");
        return false;
    }
    if (fwrite(&chunkSize, sizeof(uint32_t), 1, fd) != 1) {
        cclog_error("failed to write preview chunk size");
        return false;
    }

    // it is possible not to have a preview
    if (imageDataSize > 0) {
        // save preview bytes
        if (fwrite(imageData, imageDataSize, 1, fd) != 1) {
            cclog_error("failed to write preview bytes");
            return false;
        }
    }

    return true;
}

uint8_t chunk_v6_read_identifier(Stream *s) {
    uint8_t i;
    if (stream_read_uint8(s, &i) == false) {
        return P3S_CHUNK_ID_NONE;
    }

    if (i > P3S_CHUNK_ID_NONE && i < P3S_CHUNK_ID_MAX) {
        return i;
    }

    return P3S_CHUNK_ID_NONE;
}

uint32_t chunk_v6_read_size(Stream *s) {
    uint32_t i;
    if (stream_read_uint32(s, &i) == false) {
        cclog_error("failed to read v6 size");
        return 0;
    }
    // cclog_debug("> read v6 size: %d", i);
    return i;
}

bool chunk_v6_read(void **chunkData, uint32_t *chunkSize, uint32_t *uncompressedSize, Stream *s) {

    uint32_t _chunkSize = 0;
    uint8_t _isCompressed = 0;
    uint32_t _uncompressedSize = 0;

    // read chunk header, chunk ID should be read already at this point
    if (stream_read_uint32(s, &_chunkSize) == false) {
        return false;
    }
    if (stream_read_uint8(s, &_isCompressed) == false) {
        return false;
    }
    if (stream_read_uint32(s, &_uncompressedSize) == false) {
        return false;
    }

    if (_chunkSize == 0 || _uncompressedSize == 0) {
        return false;
    }

    // read chunk data
    void *_chunkData = malloc(_chunkSize);
    if (stream_read(s, _chunkData, _chunkSize, 1) == false) {
        free(_chunkData);
        return false;
    }

    // uncompress if required by this chunk
    if (_isCompressed != 0) {
        uLong resultSize = _uncompressedSize;
        void *uncompressedData = malloc(_uncompressedSize);
        if (uncompress(uncompressedData, &resultSize, _chunkData, _chunkSize) != Z_OK) {
            free(uncompressedData);
            free(_chunkData);
            return false;
        }
        free(_chunkData);

        *chunkData = uncompressedData;
    } else {
        *chunkData = _chunkData;
    }
    *chunkSize = _chunkSize;
    *uncompressedSize = _uncompressedSize;
    return true;
}

// skips a chunk with v5 header (only chunkSize as uint32_t)
uint32_t chunk_v6_with_v5_header_skip(Stream *s) {
    uint32_t chunkSize = chunk_v6_read_size(s);
    uint32_t skippedBytes = 4;

    stream_skip(s, chunkSize);
    skippedBytes += chunkSize;
    return skippedBytes;
}

uint32_t chunk_v6_skip(Stream *s) {
    uint32_t chunkSize = chunk_v6_read_size(s);
    stream_skip(s, chunkSize + CHUNK_V6_HEADER_NO_ID_SKIP_SIZE);
    return CHUNK_V6_HEADER_NO_ID_SIZE + chunkSize;
}

ColorPalette *chunk_v6_read_palette_data(void *cursor, ColorAtlas *colorAtlas, bool isLegacy) {
    uint16_t colorCount = 0;

    if (isLegacy) {
        // number of rows (unused)
        cursor = (void *)((uint8_t *)cursor + 1);
        // number of columns (unused)
        cursor = (void *)((uint8_t *)cursor + 1);
        // color count
        colorCount = *((uint16_t *)cursor);
        cursor = (void *)((uint16_t *)cursor + 1);
        // default color (unused)
        cursor = (void *)((uint8_t *)cursor + 1);
        // default background color (unused)
        cursor = (void *)((uint8_t *)cursor + 1);
    } else {
        // color count
        colorCount = *((uint8_t *)cursor);
        cursor = (void *)((uint8_t *)cursor + 1);
    }
    // pointer to colors
    RGBAColor *colors = (RGBAColor *)cursor;
    cursor = (void *)((RGBAColor *)cursor + colorCount);
    // pointer to emissive flags
    bool *emissive = (bool *)cursor;

    ColorPalette *palette = color_palette_new_from_data(colorAtlas,
                                                        (uint8_t)minimum(colorCount, UINT8_MAX),
                                                        colors,
                                                        emissive);
    return palette;
}

uint32_t chunk_v6_read_palette(Stream *s,
                               ColorAtlas *colorAtlas,
                               ColorPalette **palette,
                               bool isLegacy) {

    if (palette == NULL) {
        cclog_error("can't read palette without pointer to store it");
        return 0;
    }

    if (*palette != NULL) {
        color_palette_release(*palette);
        *palette = NULL;
    }

    /// read file
    void *chunkData = NULL;
    uint32_t chunkSize = 0;
    uint32_t uncompressedSize = 0;
    if (chunk_v6_read(&chunkData, &chunkSize, &uncompressedSize, s) == false) {
        cclog_error("failed to read palette");
        return 0;
    }

    *palette = chunk_v6_read_palette_data(chunkData, colorAtlas, isLegacy);
    free(chunkData);

    return CHUNK_V6_HEADER_NO_ID_SIZE + chunkSize;
}

uint32_t chunk_v6_read_palette_id(Stream *s, uint8_t *paletteID) {
    /// read file to get size, but this chunk is now unused
    void *chunkData = NULL;
    uint32_t chunkSize = 0;
    uint32_t uncompressedSize = 0;
    if (chunk_v6_read(&chunkData, &chunkSize, &uncompressedSize, s) == false) {
        return 0;
    }

    *paletteID = *((uint8_t *)chunkData);

    free(chunkData);

    return CHUNK_V6_HEADER_NO_ID_SIZE + chunkSize;
}

uint32_t chunk_v6_read_shape_process_blocks(void *cursor,
                                            Shape *shape,
                                            uint16_t w,
                                            uint16_t h,
                                            uint16_t d,
                                            uint8_t paletteID,
                                            ColorPalette *shrinkPalette) {
    uint32_t size = 0;
    memcpy(&size, cursor, sizeof(uint32_t));
    cursor = (void *)((uint32_t *)cursor + 1);
    SHAPE_COLOR_INDEX_INT_T colorIndex;
    ColorPalette *palette = shape_get_palette(shape);
    for (SHAPE_COORDS_INT_T x = 0; x < w; x++) { // shape blocks
        for (SHAPE_COORDS_INT_T y = 0; y < h; y++) {
            for (SHAPE_COORDS_INT_T z = 0; z < d; z++) {
                colorIndex = *((SHAPE_COLOR_INDEX_INT_T *)cursor);
                cursor = (void *)((SHAPE_COLOR_INDEX_INT_T *)cursor + 1);

                if (colorIndex == SHAPE_COLOR_INDEX_AIR_BLOCK) { // no cube
                    continue;
                }

                bool success = true;
                // translate & shrink to a shape palette w/ only used colors if,
                // 1) octree was serialized w/ a palette ID using any of the default palettes
                if (paletteID == PALETTE_ID_IOS_ITEM_EDITOR_LEGACY) {
                    success = color_palette_check_and_add_default_color_pico8p(palette,
                                                                               colorIndex,
                                                                               &colorIndex);
                } else if (paletteID == PALETTE_ID_2021) {
                    success = color_palette_check_and_add_default_color_2021(palette,
                                                                             colorIndex,
                                                                             &colorIndex);
                }
                // 2) octree was serialized w/ a palette that exceeds max size
                else if (shrinkPalette != NULL) {
                    RGBAColor color = color_palette_get_color(shrinkPalette, colorIndex);
                    success = color_palette_check_and_add_color(palette, color, &colorIndex, false);
                }
                if (success == false) {
                    colorIndex = 0;
                }

                shape_add_block(shape, colorIndex, x, y, z, false);
            }
        }
    }
    color_palette_clear_lighting_dirty(palette);

    return size + sizeof(uint32_t);
}

uint32_t chunk_v6_read_shape(Stream *s,
                             Shape **shape,
                             DoublyLinkedList *shapes,
                             const LoadShapeSettings *const shapeSettings,
                             ColorAtlas *colorAtlas,
                             ColorPalette *filePalette,
                             uint8_t paletteID,
                             ColorPalette **rootShapePalette) {
    if (shapeSettings == NULL) {
        cclog_error("tried to load shape without shape settings");
        return 0;
    }

    /// read file
    void *chunkData = NULL;
    uint32_t chunkSize = 0;
    uint32_t uncompressedSize = 0;
    if (chunk_v6_read(&chunkData, &chunkSize, &uncompressedSize, s) == false) {
        cclog_error("failed to read shape");
        return 0;
    }

    // no need to read if shape return parameter is NULL
    if (shape == NULL) {
        cclog_error("shape pointer is null");
        free(chunkData);
        return CHUNK_V6_HEADER_NO_ID_SIZE + chunkSize;
    }

    if (*shape != NULL) {
        shape_release(*shape);
        *shape = NULL;
    }

    /// get shape data
    void *cursor = chunkData;
    void *shapeBlocksCursor = NULL;

    uint32_t totalSizeRead = 0;
    uint32_t sizeRead = 0;
    uint32_t lightingDataSizeRead = 0;

    uint8_t chunkID;
    MapStringFloat3 *pois = map_string_float3_new();
    MapStringFloat3 *pois_rotation = map_string_float3_new();
    VERTEX_LIGHT_STRUCT_T *lightingData = NULL;
    ColorPalette *palette = NULL;

    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t depth = 0;

    uint16_t shapeId = 1;
    uint16_t shapeParentId = 0;

    bool hasCustomCollisionBox = false;
    float3 collisionBoxMin = float3_zero;
    float3 collisionBoxMax = float3_zero;
    uint8_t isHiddenSelf = false;

    LocalTransform localTransform;
    memset(&localTransform, 0, sizeof(LocalTransform));
    localTransform.scale.x = 1;
    localTransform.scale.y = 1;
    localTransform.scale.z = 1;
    char *name = NULL;
    float3 pivot = {0.0f, 0.0f, 0.0f};
    bool hasPivot = false;

    while (totalSizeRead < uncompressedSize) {
        chunkID = *((uint8_t *)cursor);
        cursor = (void *)((uint8_t *)cursor + 1);
        totalSizeRead += 1; // size of chunk id
        switch (chunkID) {
            case P3S_CHUNK_ID_SHAPE_ID: {
                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // shape id chunk size
                cursor = (void *)((uint32_t *)cursor + 1);
                memcpy(&shapeId, cursor, sizeof(uint16_t));
                cursor = (void *)((uint16_t *)cursor + 1);
                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                break;
            }
            case P3S_CHUNK_ID_SHAPE_PARENT_ID: {
                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // shape id chunk size
                cursor = (void *)((uint32_t *)cursor + 1);
                memcpy(&shapeParentId, cursor, sizeof(uint16_t));
                cursor = (void *)((uint16_t *)cursor + 1);
                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                break;
            }
            case P3S_CHUNK_ID_SHAPE_TRANSFORM: {
                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // shape id chunk size
                cursor = (void *)((uint32_t *)cursor + 1);
                memcpy(&localTransform, cursor, sizeof(LocalTransform));
                cursor = (void *)((LocalTransform *)cursor + 1);
                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                break;
            }
            case P3S_CHUNK_ID_SHAPE_PIVOT: {
                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // shape id chunk size
                cursor = (void *)((uint32_t *)cursor + 1);
                memcpy(&pivot, cursor, sizeof(float3));
                cursor = (void *)((float3 *)cursor + 1);
                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                hasPivot = true;
                break;
            }
            case P3S_CHUNK_ID_SHAPE_PALETTE: {
                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // shape palette chunk size
                cursor = (void *)((uint32_t *)cursor + 1);

                palette = chunk_v6_read_palette_data(cursor, colorAtlas, false);
                cursor = (void *)((uint8_t *)cursor + sizeRead);

                paletteID = PALETTE_ID_CUSTOM;

                if (*rootShapePalette == NULL) {
                    *rootShapePalette = palette; // for [MULTI] file, root shape palette may be
                                                 // shared
                }

                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                break;
            }
            case P3S_CHUNK_ID_OBJECT_COLLISION_BOX: {
                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // shape id chunk size
                cursor = (void *)((uint32_t *)cursor + 1);
                memcpy(&collisionBoxMin, cursor, sizeof(float3));
                cursor = (void *)((float3 *)cursor + 1);
                memcpy(&collisionBoxMax, cursor, sizeof(float3));
                cursor = (void *)((float3 *)cursor + 1);
                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                hasCustomCollisionBox = true;
                break;
            }
            case P3S_CHUNK_ID_OBJECT_IS_HIDDEN: {
                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // object is hidden chunk size
                cursor = (void *)((uint32_t *)cursor + 1);
                memcpy(&isHiddenSelf, cursor, sizeof(uint8_t));
                cursor = (void *)((uint8_t *)cursor + 1);
                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                break;
            }
            case P3S_CHUNK_ID_SHAPE_NAME: {
                uint8_t nameLen;
                memcpy(&nameLen, cursor, sizeof(uint8_t));
                cursor = (void *)((uint8_t *)cursor + 1);
                if (name != NULL) { // shouldn't happen
                    free(name);
                }
                name = malloc(nameLen + 1);
                if (name == NULL) {
                    cclog_error("malloc failed");
                } else {
                    memcpy(name, cursor, sizeof(char) * nameLen);
                    name[nameLen] = 0;
                }
                cursor = (void *)((uint8_t *)cursor + nameLen);
                totalSizeRead += (uint32_t)(sizeof(uint8_t) + sizeof(char) * nameLen);
                break;
            }
            case P3S_CHUNK_ID_SHAPE_SIZE: {
                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // shape size chunk size
                cursor = (void *)((uint32_t *)cursor + 1);
                memcpy(&width, cursor, sizeof(uint16_t)); // shape size X
                cursor = (void *)((uint16_t *)cursor + 1);
                memcpy(&height, cursor, sizeof(uint16_t)); // shape size Y
                cursor = (void *)((uint16_t *)cursor + 1);
                memcpy(&depth, cursor, sizeof(uint16_t)); // shape size Y
                cursor = (void *)((uint16_t *)cursor + 1);

                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);

                // size is known, now is a good time to create the shape
                *shape = shape_make_2(shapeSettings->isMutable);
                break;
            }
            case P3S_CHUNK_ID_SHAPE_BLOCKS: {
                // Palette and size are required to read blocks, storing blocks position to process
                // them later
                shapeBlocksCursor = cursor;

                // shape blocks chunk size
                memcpy(&sizeRead, cursor, sizeof(uint32_t));
                cursor = (void *)((uint32_t *)cursor + 1);

                // skip chunk for now
                cursor = (void *)((uint8_t *)cursor + sizeRead);
                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                break;
            }
            case P3S_CHUNK_ID_SHAPE_POINT: {
                uint8_t nameLen = 0;
                char *nameStr = NULL;
                float3 *poi = float3_new(0, 0, 0);

                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // shape POI chunk size
                cursor = (void *)((uint32_t *)cursor + 1);

                memcpy(&nameLen, cursor, sizeof(uint8_t)); // shape POI name length
                cursor = (void *)((uint8_t *)cursor + 1);

                nameStr = (char *)malloc(nameLen + 1); // +1 for null terminator
                if (nameStr == NULL) {
                    cclog_error("malloc failed");
                } else {
                    memcpy(nameStr, cursor, nameLen); // shape POI name
                    nameStr[nameLen] = 0;             // add null terminator
                }
                cursor = (void *)((char *)cursor + nameLen);

                memcpy(&(poi->x), cursor, sizeof(float)); // shape POI X
                cursor = (void *)((float *)cursor + 1);

                memcpy(&(poi->y), cursor, sizeof(float)); // shape POI Y
                cursor = (void *)((float *)cursor + 1);

                memcpy(&(poi->z), cursor, sizeof(float)); // shape POI Z
                cursor = (void *)((float *)cursor + 1);

                if (nameStr != NULL) {
                    map_string_float3_set_key_value(pois, nameStr, poi);
                    free(nameStr);
                }

                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                break;
            }
            case P3S_CHUNK_ID_SHAPE_POINT_ROTATION: {
                uint8_t nameLen = 0;
                char *nameStr = NULL;
                float3 *poi = float3_new(0, 0, 0);

                memcpy(&sizeRead, cursor, sizeof(uint32_t)); // shape POI chunk size
                cursor = (void *)((uint32_t *)cursor + 1);

                memcpy(&nameLen, cursor, sizeof(uint8_t)); // shape POI name length
                cursor = (void *)((uint8_t *)cursor + 1);

                nameStr = (char *)malloc(nameLen + 1); // +1 for null terminator
                if (nameStr == NULL) {
                    cclog_error("malloc failed");
                } else {
                    memcpy(nameStr, cursor, nameLen); // shape POI name
                    nameStr[nameLen] = 0;             // add null terminator
                }
                cursor = (void *)((char *)cursor + nameLen);

                memcpy(&(poi->x), cursor, sizeof(float)); // shape POI X
                cursor = (void *)((float *)cursor + 1);

                memcpy(&(poi->y), cursor, sizeof(float)); // shape POI Y
                cursor = (void *)((float *)cursor + 1);

                memcpy(&(poi->z), cursor, sizeof(float)); // shape POI Z
                cursor = (void *)((float *)cursor + 1);

                if (nameStr != NULL) {
                    map_string_float3_set_key_value(pois_rotation, nameStr, poi);
                    free(nameStr);
                }

                totalSizeRead += sizeRead + (uint32_t)sizeof(uint32_t);
                break;
            }
#if GLOBAL_LIGHTING_BAKE_READ_ENABLED
            case P3S_CHUNK_ID_SHAPE_BAKED_LIGHTING: {
                // shape baked lighting chunk size
                memcpy(&lightingDataSizeRead, cursor, sizeof(uint32_t));
                cursor = (void *)((uint32_t *)cursor + 1);

                totalSizeRead += lightingDataSizeRead + (uint32_t)sizeof(uint32_t);

                if (shapeSettings->lighting) {
                    if (lightingData != NULL) { // shouldn't happen
                        free(lightingData);
                    }
                    lightingData = (VERTEX_LIGHT_STRUCT_T *)malloc(lightingDataSizeRead);
                    if (lightingData == NULL) {
                        break;
                    }

                    memcpy(lightingData, cursor, lightingDataSizeRead);
                    cursor = (void *)((char *)cursor + lightingDataSizeRead);
                }
            }
#endif
            default: // shape sub chunks we don't need to read
            {
                /*
                 #define P3S_CHUNK_ID_SELECTED_COLOR 8
                 #define P3S_CHUNK_ID_SELECTED_BACKGROUND_COLOR 9
                 #define P3S_CHUNK_ID_CAMERA 10
                 #define P3S_CHUNK_ID_DIRECTIONAL_LIGHT 11
                 #define P3S_CHUNK_ID_SOURCE_METADATA 12
                 #define P3S_CHUNK_ID_GENERAL_RENDERING_OPTIONS 14
                 */
                // sub chunk header size + sub chunk data size
                if (uncompressedSize >= totalSizeRead &&
                    uncompressedSize - totalSizeRead >= sizeof(uint32_t)) {
                    sizeRead = CHUNK_V6_HEADER_NO_ID_SIZE + *((uint32_t *)cursor);
                    // advance cursor
                    cursor = (void *)((char *)cursor + sizeRead);

                    totalSizeRead += sizeRead;
                } else {
                    totalSizeRead = uncompressedSize; // end it
                }
                break;
            }
        }
    }

    if (*shape == NULL) {
        free(lightingData);
        free(name);
        free(palette);
        map_string_float3_free(pois);
        map_string_float3_free(pois_rotation);
        free(chunkData);
        cclog_error("error while reading shape : no shape were created");
        return 0;
    }

    // Compatibility modes (see comment in serialization_load_assets_v6):
    // [MULTI] Use sub-chunk palette if it exists, else use shared palette, ignore file palette
    // [SINGLE] If file palette exists, use it as shape palette (optionally shrinked)
    // [LEGACY] No file palette, legacy palette ID will be used (shrinked)
    bool shrinkPalette = false;
    if (*rootShapePalette != NULL || palette != NULL) { // [MULTI]
        if (palette != NULL) {                          // individual palette
            shape_set_palette(*shape, palette, false);
        } else { // shared palette
            shape_set_palette(*shape, *rootShapePalette, true);
        }
        paletteID = PALETTE_ID_CUSTOM;
    } else if (filePalette != NULL) { // [SINGLE]
        shrinkPalette = color_palette_get_count(filePalette) >= SHAPE_COLOR_INDEX_MAX_COUNT;
        shape_set_palette(*shape,
                          shrinkPalette ? color_palette_new(colorAtlas)
                                        : color_palette_new_copy(filePalette),
                          false);
        paletteID = PALETTE_ID_CUSTOM;
    } else { // [LEGACY]
        shape_set_palette(*shape, color_palette_new(colorAtlas), false);
        vx_assert(paletteID != PALETTE_ID_CUSTOM); // from caller, reading legacy chunks at the root
    }

    // process blocks now
    if (shapeBlocksCursor != NULL) {
        chunk_v6_read_shape_process_blocks(shapeBlocksCursor,
                                           *shape,
                                           width,
                                           height,
                                           depth,
                                           paletteID,
                                           shrinkPalette ? filePalette : NULL);
    }

    free(chunkData);

    float3 f3;

    // set shape POIs
    MapStringFloat3Iterator *it = map_string_float3_iterator_new(pois);
    while (map_string_float3_iterator_is_done(it) == false) {
        float3 *value = map_string_float3_iterator_current_value(it);
        float3_copy(&f3, value);
        shape_set_point_of_interest(*shape, map_string_float3_iterator_current_key(it), &f3);
        map_string_float3_iterator_next(it);
    }
    map_string_float3_iterator_free(it);
    map_string_float3_free(pois);

    // set shape points (rotation)
    it = map_string_float3_iterator_new(pois_rotation);
    while (map_string_float3_iterator_is_done(it) == false) {
        float3 *value = map_string_float3_iterator_current_value(it);
        float3_copy(&f3, value);
        shape_set_point_rotation(*shape, map_string_float3_iterator_current_key(it), &f3);
        map_string_float3_iterator_next(it);
    }
    map_string_float3_iterator_free(it);
    map_string_float3_free(pois_rotation);

    // set shape lighting data
    if (shapeSettings->lighting) {
        if (lightingData == NULL) {
            cclog_warning("shape uses lighting but no baked lighting found");
        } else if (lightingDataSizeRead !=
                   (uint32_t)(width * height * depth * (uint16_t)sizeof(VERTEX_LIGHT_STRUCT_T))) {
            cclog_warning("shape uses lighting but does not match lighting data size");
            free(lightingData);
        } else {
            shape_set_lighting_data_from_blob(*shape,
                                              lightingData,
                                              coords3_zero,
                                              (SHAPE_COORDS_INT3_T){(SHAPE_COORDS_INT_T)width,
                                                                    (SHAPE_COORDS_INT_T)height,
                                                                    (SHAPE_COORDS_INT_T)depth});
        }
    } else if (lightingData != NULL) {
        cclog_warning("shape baked lighting data discarded");
        free(lightingData);
    }

    doubly_linked_list_push_last(shapes, *shape);
    if (shapes) {
        int32_t parentIndex = shapeParentId - 1;
        Shape *parent = (Shape *)doubly_linked_list_node_pointer(
            doubly_linked_list_node_at_index(shapes, (size_t)parentIndex));
        if (parentIndex >= 0 && parent) {
            shape_set_parent(*shape, shape_get_root_transform(parent), false);
            shape_set_local_position(*shape,
                                     localTransform.position.x,
                                     localTransform.position.y,
                                     localTransform.position.z);
            shape_set_local_rotation_euler(*shape,
                                           localTransform.rotation.x,
                                           localTransform.rotation.y,
                                           localTransform.rotation.z);
            shape_set_local_scale(*shape,
                                  localTransform.scale.x,
                                  localTransform.scale.y,
                                  localTransform.scale.z);
        }
    }

    if (hasPivot) {
        shape_set_pivot(*shape, pivot.x, pivot.y, pivot.z);
    } else {
        shape_reset_pivot_to_center(*shape);
    }

    if (name != NULL) {
        transform_set_name(shape_get_root_transform(*shape), name);
        free(name);
        name = NULL;
    }

    if (hasCustomCollisionBox) {
        RigidBody *rb;
        transform_ensure_rigidbody(shape_get_root_transform(*shape),
                                   RigidbodyMode_Static,
                                   PHYSICS_GROUP_DEFAULT_OBJECT,
                                   PHYSICS_COLLIDESWITH_DEFAULT_OBJECT,
                                   &rb);

        // construct new box value
        Box newCollider = *rigidbody_get_collider(rb);
        newCollider.min = collisionBoxMin;
        newCollider.max = collisionBoxMax;

        // set the new box using
        rigidbody_set_collider(rb, &newCollider, true);
    }

    Transform *const root = shape_get_root_transform(*shape);
    if (root) {
        transform_set_hidden_self(root, isHiddenSelf == 1);
    }

    return CHUNK_V6_HEADER_NO_ID_SIZE + chunkSize;
}

//
uint32_t chunk_v6_read_preview_image(Stream *s, void **imageData, uint32_t *size) {
    uint32_t chunkSize = chunk_v6_read_size(s);
    if (chunkSize == 0) {
        cclog_error("can't read preview image chunk size (v6)");
        return 0;
    }

    // read preview data
    void *previewData = malloc(chunkSize);

    if (previewData == NULL) {
        // error
        cclog_error("failed to allocate preview data buffer");
        return 0;
    }
    if (stream_read(s, previewData, 1, chunkSize) == false) {
        // error
        cclog_error("failed to read preview data");
        free(previewData);
        return 0;
    }
    // success
    *size = chunkSize;
    *imageData = previewData;

    return chunkSize + 4;
}

static bool write_chunk_in_buffer(void *destBuffer,
                                  const uint8_t chunkID,
                                  const bool isCompressed,
                                  const void *chunkWriteData,
                                  const uint32_t chunkCompressedDataSize,
                                  const uint32_t chunkUncompressedDataSize,
                                  uint32_t *externCursor) {

    // write chunk in destination buffer
    uint8_t *cursor = destBuffer;

    // chunk header
    memcpy(cursor, &chunkID, sizeof(uint8_t));
    cursor += sizeof(uint8_t);

    memcpy(cursor, &chunkCompressedDataSize, sizeof(uint32_t));
    cursor += sizeof(uint32_t);

    memcpy(cursor, &isCompressed, sizeof(uint8_t));
    cursor += sizeof(uint8_t);

    memcpy(cursor, &chunkUncompressedDataSize, sizeof(uint32_t));
    cursor += sizeof(uint32_t);

    // chunk data
    const uint32_t chunkWriteSize = isCompressed ? chunkCompressedDataSize
                                                 : chunkUncompressedDataSize;
    memcpy(cursor, chunkWriteData, chunkWriteSize);
    cursor += chunkWriteSize;

    if (externCursor != NULL) {
        *externCursor += (uint32_t)(cursor - (uint8_t *)destBuffer);
    }

    return true;
}

///
/// If externCursor is not NULL, it is incremented by the number of bytes written.
bool write_preview_chunk_in_buffer(void *destBuffer,
                                   const void *previewBytes,
                                   const uint32_t previewBytesCount,
                                   uint32_t *externCursor) {

    if (destBuffer == NULL) {
        return false;
    }
    if (previewBytes == NULL) {
        return false;
    }

    if (previewBytesCount == 0) {
        return false;
    }

    const uint8_t chunkID = P3S_CHUNK_ID_PREVIEW;
    const void *data = previewBytes;
    const uint32_t size = previewBytesCount;

    // write chunk in destination buffer
    uint8_t *cursor = destBuffer;

    // chunk header
    memcpy(cursor, &chunkID, sizeof(uint8_t));
    cursor += sizeof(uint8_t);

    memcpy(cursor, &size, sizeof(uint32_t));
    cursor += sizeof(uint32_t);

    // chunk data
    memcpy(cursor, data, size);
    cursor += size;

    if (externCursor != NULL) {
        *externCursor += (uint32_t)(cursor - (uint8_t *)destBuffer);
    }

    return true;
}

bool chunk_v6_shape_create_and_write_uncompressed_buffer(const Shape *shape,
                                                         uint16_t shapeId,
                                                         uint16_t shapeParentId,
                                                         const ColorPalette *sharedPalette,
                                                         uint32_t *uncompressedSize,
                                                         void **uncompressedData) {
    if (uncompressedSize == NULL) {
        return false;
    }
    if (uncompressedData == NULL) {
        return false;
    }

    if (*uncompressedData != NULL) {
        free(*uncompressedData);
    }

    const Block *block = NULL;
    MapStringFloat3Iterator *it = NULL;

    // we only have to write blocks that are in the bounding box
    // using boundingBox->min to offset blocks at 0,0,0 when writing
    // blocks, POIs, and lightingData
    SHAPE_COORDS_INT3_T start, end; // 'end' (bbMax) is non-inclusive
    shape_get_model_aabb_2(shape, &start, &end);

    int3 shapeSize;
    shape_get_bounding_box_size(shape, &shapeSize);

    const uint32_t blockCount = (uint32_t)(shapeSize.x * shapeSize.y * shapeSize.z);

#if GLOBAL_LIGHTING_BAKE_WRITE_ENABLED
    const bool hasLighting = shape_uses_baked_lighting(shape);
#else
    const bool hasLighting = false;
#endif

    // hasCustomCollisionBox
    RigidBody *rb = shape_get_rigidbody(shape);
    const Box *collider = rb != NULL ? rigidbody_get_collider(rb) : NULL;
    bool hasCustomCollisionBox = collider != NULL && rigidbody_is_collider_custom_set(rb);

    // is hidden
    Transform *t = shape_get_root_transform(shape);
    uint8_t isHidden = transform_is_hidden_self(t) ? 1 : 0;

    // get palette chunk, if not sharing palette w/ root shape
    uint32_t shapePaletteSize = 0;
    void *shapePaletteData = NULL;
    SHAPE_COLOR_INDEX_INT_T *paletteMapping = NULL;
    if (shapeParentId == 0 || shape_get_palette(shape) != sharedPalette) {
        _chunk_v6_palette_create_and_write_uncompressed_buffer(shape_get_palette(shape),
                                                               &shapePaletteSize,
                                                               &shapePaletteData,
                                                               &paletteMapping);
    }

    const char *name = transform_get_name(shape_get_root_transform(shape));
    uint8_t nameLen = 0;
    if (name != NULL) {
        nameLen = (uint8_t)strlen(name);
    }

    // prepare buffer with shape chunk uncompressed data

    // shape sub-chunks size
    uint32_t subheaderSize = sizeof(uint8_t) + sizeof(uint32_t);
    uint32_t shapeSizeSize = 3 * sizeof(uint16_t);
    uint32_t shapeIdSize = sizeof(uint16_t);
    uint32_t shapeParentIdSize = sizeof(uint16_t);
    uint32_t shapePivotSize = sizeof(float3);
    uint32_t objectCollisionBoxSize = sizeof(float3) * 2;
    uint32_t objectIsHiddenSelfSize = sizeof(uint8_t);
    uint32_t shapeLocalTransformSize = sizeof(LocalTransform);
    uint32_t shapeBlocksSize = blockCount * sizeof(uint8_t);
    uint32_t shapeLightingSize = blockCount * sizeof(VERTEX_LIGHT_STRUCT_T);
    uint32_t nameLenSize = sizeof(uint8_t);

    // Point positions sub-chunks collective size /!\ the name length can vary
    // TODO store this in a list to avoid recomputing it later
    uint32_t shapePointPositionsSize = 0, shapePointPositionsCount = 0;
    it = shape_get_poi_iterator(shape);
    while (map_string_float3_iterator_is_done(it) == false) {
        const char *key = map_string_float3_iterator_current_key(it);
        const float3 *f3 = map_string_float3_iterator_current_value(it);

        if (key == NULL || f3 == NULL) {
            continue;
        }

        // name length w/ 255 chars max
        const uint32_t keyLen = CLAMP((uint32_t)strlen(key), 0, 255);

        shapePointPositionsSize += (uint32_t)sizeof(uint8_t) + keyLen + 3 * (uint32_t)sizeof(float);
        shapePointPositionsCount++;

        map_string_float3_iterator_next(it);
    }
    map_string_float3_iterator_free(it);

    // Point rotations sub-chunks collective size /!\ the name length can vary
    // TODO store this in a list to avoid recomputing it later
    uint32_t shapePointRotationsSize = 0, shapePointRotationsCount = 0;
    it = shape_get_point_rotation_iterator(shape);
    while (map_string_float3_iterator_is_done(it) == false) {
        const char *key = map_string_float3_iterator_current_key(it);
        const float3 *f3 = map_string_float3_iterator_current_value(it);

        if (key == NULL || f3 == NULL) {
            continue;
        }

        // name length w/ 255 chars max
        const uint32_t keyLen = CLAMP((uint32_t)strlen(key), 0, 255);

        shapePointRotationsSize += (uint32_t)sizeof(uint8_t) + keyLen + 3 * (uint32_t)sizeof(float);
        shapePointRotationsCount++;

        map_string_float3_iterator_next(it);
    }
    map_string_float3_iterator_free(it);

    // allocate for uncompressed data
    *uncompressedSize = subheaderSize + shapeSizeSize +
                        (shapeId > 0 ? subheaderSize + shapeIdSize : 0) +
                        (shapeParentId > 0 ? subheaderSize + shapeParentIdSize + subheaderSize +
                                                 shapeLocalTransformSize
                                           : 0) +
                        subheaderSize + shapePivotSize + subheaderSize + shapeBlocksSize +
                        (shapePaletteSize > 0 ? subheaderSize + shapePaletteSize : 0) +
                        (hasCustomCollisionBox ? subheaderSize + objectCollisionBoxSize : 0) +
                        (isHidden == 1 ? subheaderSize + objectIsHiddenSelfSize : 0) +
                        shapePointPositionsCount * subheaderSize + shapePointPositionsSize +
                        shapePointRotationsCount * subheaderSize + shapePointRotationsSize +
                        (hasLighting ? subheaderSize + shapeLightingSize : 0) +
                        (nameLen > 0 ? subheaderSize + nameLenSize + nameLen : 0);

    *uncompressedData = malloc(*uncompressedSize);
    if (*uncompressedData == NULL) {
        free(shapePaletteData);
        return false;
    }

    void *cursor = *uncompressedData;

    // shape size sub-chunk
    const uint8_t chunk_id_shape_size = P3S_CHUNK_ID_SHAPE_SIZE;
    memcpy(cursor, &chunk_id_shape_size, sizeof(uint8_t)); // shape size chunk ID
    cursor = (void *)((uint8_t *)cursor + 1);

    memcpy(cursor, &shapeSizeSize, sizeof(uint32_t)); // shape size chunk size
    cursor = (void *)((uint32_t *)cursor + 1);

    memcpy(cursor, &(shapeSize.x), sizeof(uint16_t)); // shape size X
    cursor = (void *)((uint16_t *)cursor + 1);

    memcpy(cursor, &(shapeSize.y), sizeof(uint16_t)); // shape size Y
    cursor = (void *)((uint16_t *)cursor + 1);

    memcpy(cursor, &(shapeSize.z), sizeof(uint16_t)); // shape size Z
    cursor = (void *)((uint16_t *)cursor + 1);

    // shape id sub-chunk
    if (shapeId != 0) {
        const uint8_t chunk_id_shape_id = P3S_CHUNK_ID_SHAPE_ID;
        memcpy(cursor, &chunk_id_shape_id, sizeof(uint8_t)); // shape id chunk ID
        cursor = (void *)((uint8_t *)cursor + 1);

        memcpy(cursor, &shapeIdSize, sizeof(uint32_t)); // size chunk ID
        cursor = (void *)((uint32_t *)cursor + 1);

        memcpy(cursor, &shapeId, sizeof(uint16_t));
        cursor = (void *)((uint16_t *)cursor + 1);
    }

    // shape parent id sub-chunk and transform
    if (shapeParentId != 0) {
        const uint8_t chunk_id_shape_parent_id = P3S_CHUNK_ID_SHAPE_PARENT_ID;
        memcpy(cursor, &chunk_id_shape_parent_id, sizeof(uint8_t)); // shape parent id chunk ID
        cursor = (void *)((uint8_t *)cursor + 1);

        memcpy(cursor, &shapeParentIdSize, sizeof(uint32_t)); // size chunk parent ID
        cursor = (void *)((uint32_t *)cursor + 1);

        memcpy(cursor, &shapeParentId, sizeof(uint16_t));
        cursor = (void *)((uint16_t *)cursor + 1);

        const uint8_t chunk_id_shape_transform = P3S_CHUNK_ID_SHAPE_TRANSFORM;
        memcpy(cursor, &chunk_id_shape_transform, sizeof(uint8_t)); // shape transform chunk ID
        cursor = (void *)((uint8_t *)cursor + 1);

        memcpy(cursor, &shapeLocalTransformSize, sizeof(uint32_t)); // size chunk transform
        cursor = (void *)((uint32_t *)cursor + 1);

        LocalTransform localTransform;
        const float3 *position = shape_get_local_position(shape);
        localTransform.position.x = position->x;
        localTransform.position.y = position->y;
        localTransform.position.z = position->z;
        shape_get_local_rotation_euler(shape, &localTransform.rotation);
        const float3 *scale = shape_get_local_scale(shape);
        localTransform.scale.x = scale->x;
        localTransform.scale.y = scale->y;
        localTransform.scale.z = scale->z;
        memcpy(cursor, &localTransform, sizeof(LocalTransform));
        cursor = (void *)((LocalTransform *)cursor + 1);
    }

    const uint8_t chunk_id_shape_pivot = P3S_CHUNK_ID_SHAPE_PIVOT;
    memcpy(cursor, &chunk_id_shape_pivot, sizeof(uint8_t)); // shape pivot chunk ID
    cursor = (void *)((uint8_t *)cursor + 1);

    memcpy(cursor, &shapePivotSize, sizeof(uint32_t)); // size chunk pivot
    cursor = (void *)((uint32_t *)cursor + 1);

    float3 pivot = shape_get_pivot(shape);
    pivot.x -= start.x;
    pivot.y -= start.y;
    pivot.z -= start.z;

    memcpy(cursor, &pivot, sizeof(float3));
    cursor = (void *)((float3 *)cursor + 1);

    if (hasCustomCollisionBox) {
        const uint8_t chunk_id_object_collision_box = P3S_CHUNK_ID_OBJECT_COLLISION_BOX;
        memcpy(cursor,
               &chunk_id_object_collision_box,
               sizeof(uint8_t)); // object collision box chunk ID
        cursor = (void *)((uint8_t *)cursor + 1);

        memcpy(cursor, &objectCollisionBoxSize, sizeof(uint32_t)); // size chunk collision box
        cursor = (void *)((uint32_t *)cursor + 1);

        memcpy(cursor, &collider->min, sizeof(float3));
        cursor = (void *)((float3 *)cursor + 1);
        memcpy(cursor, &collider->max, sizeof(float3));
        cursor = (void *)((float3 *)cursor + 1);
    }

    if (isHidden) {
        const uint8_t chunk_id_object_is_hidden = P3S_CHUNK_ID_OBJECT_IS_HIDDEN;
        memcpy(cursor, &chunk_id_object_is_hidden, sizeof(uint8_t)); // object is hidden chunk ID
        cursor = (void *)((uint8_t *)cursor + 1);

        memcpy(cursor, &objectIsHiddenSelfSize, sizeof(uint32_t)); // size chunk is hidden
        cursor = (void *)((uint32_t *)cursor + 1);

        memcpy(cursor, &isHidden, sizeof(uint8_t));
        cursor = (void *)((uint8_t *)cursor + 1);
    }

    // shape palette sub-chunk
    if (shapePaletteSize > 0) {
        const uint8_t chunk_id_shape_palette = P3S_CHUNK_ID_SHAPE_PALETTE;
        memcpy(cursor, &chunk_id_shape_palette, sizeof(uint8_t)); // shape palette chunk ID
        cursor = (void *)((uint8_t *)cursor + 1);

        memcpy(cursor, &shapePaletteSize, sizeof(uint32_t)); // size chunk palette
        cursor = (void *)((uint32_t *)cursor + 1);

        memcpy(cursor, shapePaletteData, shapePaletteSize);
        cursor = (void *)((uint8_t *)cursor + shapePaletteSize);
        free(shapePaletteData);
    }

    // shape blocks sub-chunk
    *((uint8_t *)cursor) = P3S_CHUNK_ID_SHAPE_BLOCKS; // shape blocks chunk ID
    cursor = (void *)((uint8_t *)cursor + 1);
    *((uint32_t *)cursor) = shapeBlocksSize; // shape blocks chunk size
    cursor = (void *)((uint32_t *)cursor + 1);
    for (int x = start.x; x < end.x; ++x) { // shape blocks
        for (int y = start.y; y < end.y; ++y) {
            for (int z = start.z; z < end.z; ++z) {
                block = shape_get_block(shape,
                                        (SHAPE_COORDS_INT_T)x,
                                        (SHAPE_COORDS_INT_T)y,
                                        (SHAPE_COORDS_INT_T)z);
                if (block_is_solid(block)) {
                    *((uint8_t *)cursor) = paletteMapping != NULL
                                               ? paletteMapping[block_get_color_index(block)]
                                               : block_get_color_index(block);
                } else {
                    *((uint8_t *)cursor) = SHAPE_COLOR_INDEX_AIR_BLOCK;
                }
                cursor = (void *)((uint8_t *)cursor + 1);
            }
        }
    }

    // shape POI sub-chunks (one per POI)
    {
        it = shape_get_poi_iterator(shape);

        while (map_string_float3_iterator_is_done(it) == false) {
            const char *key = map_string_float3_iterator_current_key(it);
            const float3 *f3 = map_string_float3_iterator_current_value(it);

            if (key == NULL || f3 == NULL) {
                cclog_error("Point position can't be written");
                continue;
            }

            // name length w/ 255 chars max
            const uint32_t keyLen = CLAMP((uint32_t)strlen(key), 0, 255);

            const uint32_t chunkSize = sizeof(uint8_t) + keyLen + 3 * sizeof(float);

            // shape POI sub-chunk
            *((uint8_t *)cursor) = P3S_CHUNK_ID_SHAPE_POINT; // shape POI chunk ID
            cursor = (void *)((uint8_t *)cursor + 1);
            *((uint32_t *)cursor) = chunkSize; // shape POI chunk size
            cursor = (void *)((uint32_t *)cursor + 1);
            *((uint8_t *)cursor) = (uint8_t)keyLen; // shape POI name length
            cursor = (void *)((uint8_t *)cursor + 1);
            memcpy(cursor, key, keyLen); // shape POI name
            cursor = (void *)((char *)cursor + keyLen);
            *((float *)cursor) = f3->x - (float)(start.x); // shape POI X (empty space removed)
            cursor = (void *)((float *)cursor + 1);
            *((float *)cursor) = f3->y - (float)(start.y); // shape POI Y (empty space removed)
            cursor = (void *)((float *)cursor + 1);
            *((float *)cursor) = f3->z - (float)(start.z); // shape POI Z (empty space removed)
            cursor = (void *)((float *)cursor + 1);

            map_string_float3_iterator_next(it);
        }
        map_string_float3_iterator_free(it);
    }

    // shape points (rotation) sub-chunks (one per point)
    {
        it = shape_get_point_rotation_iterator(shape);

        while (map_string_float3_iterator_is_done(it) == false) {
            const char *key = map_string_float3_iterator_current_key(it);
            const float3 *f3 = map_string_float3_iterator_current_value(it);

            if (key == NULL || f3 == NULL) {
                cclog_error("Point rotation can't be written");
                continue;
            }

            // name length w/ 255 chars max
            const uint32_t keyLen = CLAMP((uint32_t)strlen(key), 0, 255);

            const uint32_t chunkSize = sizeof(uint8_t) + keyLen + 3 * sizeof(float);

            // shape POI sub-chunk
            *((uint8_t *)cursor) = P3S_CHUNK_ID_SHAPE_POINT_ROTATION; // shape POI chunk ID
            cursor = (void *)((uint8_t *)cursor + 1);
            *((uint32_t *)cursor) = chunkSize; // shape POI chunk size
            cursor = (void *)((uint32_t *)cursor + 1);
            *((uint8_t *)cursor) = (uint8_t)keyLen; // shape POI name length
            cursor = (void *)((uint8_t *)cursor + 1);
            memcpy(cursor, key, keyLen); // shape POI name
            cursor = (void *)((char *)cursor + keyLen);
            *((float *)cursor) = (float)(f3->x); // shape POI X
            cursor = (void *)((float *)cursor + 1);
            *((float *)cursor) = (float)(f3->y); // shape POI Y
            cursor = (void *)((float *)cursor + 1);
            *((float *)cursor) = (float)(f3->z); // shape POI Z
            cursor = (void *)((float *)cursor + 1);

            map_string_float3_iterator_next(it);
        }
        map_string_float3_iterator_free(it);
    }

    // shape baked lighting sub-chunk
    if (hasLighting) {
        // shape baked lighting chunk ID
        const uint8_t chunkIdShapeBakedLighting = P3S_CHUNK_ID_SHAPE_BAKED_LIGHTING;
        memcpy(cursor, &chunkIdShapeBakedLighting, sizeof(uint8_t));
        cursor = (void *)((uint8_t *)cursor + 1);

        // shape baked lighting chunk size
        memcpy(cursor, &shapeLightingSize, sizeof(uint32_t));
        cursor = (void *)((uint32_t *)cursor + 1);

        shape_create_lighting_data_blob(shape, &cursor);
    }

    if (nameLen > 0) {
        const uint8_t chunkIdName = P3S_CHUNK_ID_SHAPE_NAME;
        memcpy(cursor, &chunkIdName, sizeof(uint8_t));
        cursor = (void *)((uint8_t *)cursor + 1);

        memcpy(cursor, &nameLen, sizeof(uint8_t));
        cursor = (void *)((uint8_t *)cursor + 1);

        memcpy(cursor, name, nameLen * sizeof(char));
        cursor = (void *)((uint8_t *)cursor + nameLen);
    }

    return true;
}

bool chunk_v6_shape_create_and_write_compressed_buffer(const Shape *shape,
                                                       uint16_t shapeId,
                                                       uint16_t shapeParentId,
                                                       const ColorPalette *sharedPalette,
                                                       uint32_t *uncompressedSize,
                                                       uint32_t *compressedSize,
                                                       void **compressedData) {

    if (uncompressedSize == NULL) {
        return false;
    }
    if (compressedSize == NULL) {
        return false;
    }
    if (compressedData == NULL) {
        return false;
    }

    if (*compressedData != NULL) {
        free(*compressedData);
        *compressedData = NULL;
    }

    *uncompressedSize = 0;
    *compressedSize = 0;

    // first, get uncompressed data

    void *uncompressedData = NULL;

    if (chunk_v6_shape_create_and_write_uncompressed_buffer(shape,
                                                            shapeId,
                                                            shapeParentId,
                                                            sharedPalette,
                                                            uncompressedSize,
                                                            &uncompressedData) == false) {
        cclog_error("chunk_v6_shape_create_and_write_uncompressed_buffer failed");
        return false;
    }

    // compress it

    // compressBound is a zlib function making sure the buffer for compression will be large enough
    // _compressedSize here is not final, it will be known after compression.
    uLong _compressedSize = compressBound(*uncompressedSize);
    void *_compressedData = malloc(_compressedSize);
    if (_compressedData == NULL) {
        return false; // malloc failed
    }

    if (compress(_compressedData, &_compressedSize, uncompressedData, *uncompressedSize) != Z_OK) {
        free(_compressedData);
        return false;
    }

    free(uncompressedData);

    // now we have the final compressed size and data, we can pass it to our outputs.
    *compressedSize = (uint32_t)_compressedSize;
    *compressedData = malloc(*compressedSize);
    if (*compressedData == NULL) {
        free(_compressedData);
        return false;
    }
    memcpy(*compressedData, _compressedData, *compressedSize);

    free(_compressedData);

    return true;
}

void _chunk_v6_palette_create_and_write_uncompressed_buffer(
    const ColorPalette *palette,
    uint32_t *uncompressedSize,
    void **uncompressedData,
    SHAPE_COLOR_INDEX_INT_T **paletteMapping) {

    // apply internal mapping to re-order palette, get serialization mapping
    bool *emissive;
    RGBAColor *colors = color_palette_get_colors_as_array(palette, &emissive, paletteMapping);
    uint8_t colorCount = color_palette_get_ordered_count(palette);

    // prepare palette chunk uncompressed data
    *uncompressedSize = (uint32_t)(sizeof(uint8_t) + sizeof(RGBAColor) * colorCount +
                                   sizeof(bool) * colorCount);
    *uncompressedData = malloc(*uncompressedSize);
    if (*uncompressedData == NULL) {
        return;
    }
    void *cursor = *uncompressedData;

    // number of colors
    *((uint8_t *)cursor) = colorCount;
    cursor = (void *)((uint8_t *)cursor + 1);
    // colors
    memcpy(cursor, colors, sizeof(RGBAColor) * colorCount);
    cursor = (void *)((RGBAColor *)cursor + colorCount);
    // emissive flags
    memcpy(cursor, emissive, sizeof(bool) * colorCount);

    free(colors);
    free(emissive);
}

bool _chunk_v6_palette_create_and_write_compressed_buffer(
    const ColorPalette *palette,
    uint32_t *uncompressedSize,
    uint32_t *compressedSize,
    void **compressedData,
    SHAPE_COLOR_INDEX_INT_T **paletteMapping) {
    void *uncompressedData = NULL;
    if (paletteMapping != NULL) {
        *paletteMapping = NULL;
    }
    _chunk_v6_palette_create_and_write_uncompressed_buffer(palette,
                                                           uncompressedSize,
                                                           &uncompressedData,
                                                           paletteMapping);

    // zlib: compressBound estimates size so we can allocate
    uLong _compressedSize = compressBound(*uncompressedSize);
    void *_compressedData = malloc(_compressedSize);
    if (_compressedData == NULL) { // malloc failed
        free(uncompressedData);
        return false;
    }

    if (compress(_compressedData, &_compressedSize, uncompressedData, *uncompressedSize) != Z_OK) {
        free(uncompressedData);
        free(_compressedData);
        return false;
    }
    free(uncompressedData);

    // now we have the final compressed size and data
    *compressedSize = (uint32_t)_compressedSize;
    *compressedData = malloc(*compressedSize);
    if (compressedData == NULL) { // mem alloc failed
        free(_compressedData);
        return false;
    }
    memcpy(*compressedData, _compressedData, *compressedSize);

    free(_compressedData);

    return true;
}

uint32_t getChunkHeaderSize(const uint8_t chunkID) {
    uint32_t result = 0;
    switch (chunkID) {
        case P3S_CHUNK_ID_PREVIEW: {
            // v5 header
            // chunkID | chunkSize
            result = sizeof(uint8_t) + sizeof(uint32_t);
            break;
        }
        case P3S_CHUNK_ID_PALETTE:
        case P3S_CHUNK_ID_PALETTE_LEGACY:
        case P3S_CHUNK_ID_PALETTE_ID:
        case P3S_CHUNK_ID_SHAPE: {
            // v6 chunk header
            // chunkID | chunkSize | isCompressed | chunkUncompressedSize
            result = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t);
            break;
        }
        default: {
            // this should not happen
            vx_assert(false);
            return 0;
        }
    }
    return result;
}

uint32_t compute_preview_chunk_size(const uint32_t previewBytesCount) {
    return getChunkHeaderSize(P3S_CHUNK_ID_PREVIEW) + previewBytesCount;
}

uint32_t compute_shape_chunk_size(uint32_t shapeBufferDataSize) {
    return getChunkHeaderSize(P3S_CHUNK_ID_SHAPE) + shapeBufferDataSize;
}

bool create_shape_buffers(DoublyLinkedList *shapesBuffers,
                          Shape const *shape,
                          uint16_t *shapeId,
                          uint16_t shapeParentId,
                          const ColorPalette *sharedPalette,
                          uint32_t *size) {

    ShapeBuffers *currentBuffer = calloc(1, sizeof(ShapeBuffers));
    if (currentBuffer == NULL) {
        return false;
    }
    doubly_linked_list_push_last(shapesBuffers, currentBuffer);

    if (chunk_v6_shape_create_and_write_compressed_buffer(shape,
                                                          *shapeId,
                                                          shapeParentId,
                                                          sharedPalette,
                                                          &currentBuffer->shapeUncompressedDataSize,
                                                          &currentBuffer->shapeCompressedDataSize,
                                                          &currentBuffer->shapeCompressedData) ==
        false) {
        return false;
    }
    *size += compute_shape_chunk_size(currentBuffer->shapeCompressedDataSize);

    shapeParentId = *shapeId;
    (*shapeId)++;

    DoublyLinkedListNode *n = transform_get_children_iterator(shape_get_root_transform(shape));
    Transform *child = NULL;
    while (n != NULL) {
        child = (Transform *)(doubly_linked_list_node_pointer(n));
        Shape *childShape = transform_utils_get_shape(child);
        if (childShape != NULL) {
            if (create_shape_buffers(shapesBuffers,
                                     childShape,
                                     shapeId,
                                     shapeParentId,
                                     sharedPalette,
                                     size) == false) {
                return false;
            }
        }
        n = doubly_linked_list_node_next(n);
    }
    return true;
}

DoublyLinkedList *serialization_load_assets_v6(Stream *s,
                                               ColorAtlas *colorAtlas,
                                               const AssetType filterMask,
                                               const LoadShapeSettings *const shapeSettings) {

    uint8_t i;
    if (stream_read_uint8(s, &i) == false) {
        cclog_error("failed to read compression algo");
        return NULL;
    }
    P3sCompressionMethod compressionAlgo = (P3sCompressionMethod)i;

    if (compressionAlgo >= P3sCompressionMethod_COUNT) {
        cclog_error("compression algo not supported");
        return NULL;
    }

    uint32_t totalSize = 0;

    if (stream_read_uint32(s, &totalSize) == false) {
        cclog_error("failed to read total size");
        return NULL;
    }

    DoublyLinkedList *list = doubly_linked_list_new();

    // READ ALL CHUNKS UNTIL DONE

    uint32_t totalSizeRead = 0;
    uint32_t sizeRead = 0;

    uint8_t chunkID;

    bool error = false;

    // TODO: update this
    // After 0.0.48 release w/ multi-shape support, there can be 3 [compatibility modes],
    // 1) recent file, [MULTI]
    //  - palette chunk represents a standalone palette w/ no relation to any shape, could be absent
    //  - each shape has its own individual palette sub-chunk
    //  - shape palettes only contain used colors
    // 2) pre .48 file,
    //  - palette chunk represents shape palette
    //  - only 1 shape
    //  - palette may contain unused colors & legacy palettes may be shrinked
    // In this case, shape blocks may have been serialized w/ default or shape palette indices,
    // 2a) if there is a serialized palette, we consider that the octree was serialized w/ shape
    // palette indices, use it and optionally shrink it [SINGLE]
    // 2b) if not, the octree was serialized w/ default palette indices (which legacy palette
    // depends on whether or not the P3S_CHUNK_ID_PALETTE_ID exists & its value), we'll build a
    // shape palette from the used default colors [LEGACY]
    ColorPalette *serializedPalette = NULL;
    ColorPalette *rootShapePalette = NULL;
    bool serializedPaletteAssigned = false;
    uint8_t paletteID = PALETTE_ID_IOS_ITEM_EDITOR_LEGACY; // by default, pico8+ legacy colors

    DoublyLinkedList *shapes = doubly_linked_list_new();
    while (totalSizeRead < totalSize && error == false) {
        chunkID = chunk_v6_read_identifier(s);
        totalSizeRead += 1; // size of chunk id

        switch (chunkID) {
            case P3S_CHUNK_ID_NONE: {
                cclog_error("wrong chunk id found");
                error = true;
                break;
            }
            case P3S_CHUNK_ID_PALETTE_LEGACY:
            case P3S_CHUNK_ID_PALETTE: {
                // serialized palette could be for any compatibility mode (see above)
                sizeRead = chunk_v6_read_palette(s,
                                                 colorAtlas,
                                                 &serializedPalette,
                                                 chunkID == P3S_CHUNK_ID_PALETTE_LEGACY);
                paletteID = PALETTE_ID_CUSTOM;

                if (filterMask == AssetType_Any || (filterMask & AssetType_Palette) > 0) {
                    Asset *asset = malloc(sizeof(Asset));
                    if (asset == NULL) {
                        cclog_error("error while reading palette");
                        error = true;
                        break;
                    }
                    asset->ptr = serializedPalette;
                    asset->type = AssetType_Palette;
                    doubly_linked_list_push_last(list, asset);
                    serializedPaletteAssigned = true;
                }

                if (sizeRead == 0) {
                    cclog_error("error while reading palette");
                    error = true;
                    break;
                }

                totalSizeRead += sizeRead;
                break;
            }
            case P3S_CHUNK_ID_PALETTE_ID: {
                // palette ID may be used in [LEGACY] compatibility mode (see above)
                sizeRead = chunk_v6_read_palette_id(s, &paletteID);

                if (sizeRead == 0) {
                    cclog_error("error while reading palette ID");
                    error = true;
                    break;
                }

                totalSizeRead += sizeRead;
                break;
            }
            case P3S_CHUNK_ID_SHAPE: {
                Shape *shape = NULL;
                sizeRead = chunk_v6_read_shape(s,
                                               &shape,
                                               shapes,
                                               shapeSettings,
                                               colorAtlas,
                                               serializedPalette,
                                               paletteID,
                                               &rootShapePalette);

                if (sizeRead == 0) {
                    cclog_error("error while reading shape");
                    error = true;
                    break;
                }

                // shrink box once all blocks were added to update box origin
                shape_reset_box(shape);

                if (filterMask == AssetType_Any ||
                    (filterMask & (AssetType_Shape | AssetType_Object)) > 0) {
                    Asset *asset = malloc(sizeof(Asset));
                    if (asset == NULL) {
                        cclog_error("error while allocating asset (shape)");
                        error = true;
                        break;
                    }
                    asset->ptr = shape;
                    asset->type = AssetType_Shape;
                    doubly_linked_list_push_last(list, asset);
                }

                totalSizeRead += sizeRead;
                break;
            }
            default: {
                // v5 chunks we don't need to read
                totalSizeRead += chunk_v6_with_v5_header_skip(s);
                break;
            }
        }
    }

    if (serializedPalette != NULL && serializedPaletteAssigned == false) {
        color_palette_release(serializedPalette);
    }

    doubly_linked_list_free(shapes);

    if (error) {
        cclog_error("error reading file");
    }

    return list;
}
