/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%                         H   H  EEEEE  IIIII   CCCC                          %
%                         H   H  E        I    C                              %
%                         HHHHH  EEE      I    C                              %
%                         H   H  E        I    C                              %
%                         H   H  EEEEE  IIIII   CCCC                          %
%                                                                             %
%                                                                             %
%                         Read/Write Heic Image Format                        %
%                                                                             %
%                               (c) Yandex LLC                                %
%                               Anton Kortunov                                %
%                                October 2017                                 %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%
%
*/

// TODO make autodetect in configure scripts
#define MAGICKCORE_HEIC_DELEGATE 1

/*
  Include declarations.
*/
#include "MagickCore/studio.h"
#include "MagickCore/artifact.h"
#include "MagickCore/blob.h"
#include "MagickCore/blob-private.h"
#include "MagickCore/client.h"
#include "MagickCore/colorspace-private.h"
#include "MagickCore/display.h"
#include "MagickCore/exception.h"
#include "MagickCore/exception-private.h"
#include "MagickCore/image.h"
#include "MagickCore/image-private.h"
#include "MagickCore/list.h"
#include "MagickCore/magick.h"
#include "MagickCore/monitor.h"
#include "MagickCore/monitor-private.h"
#include "MagickCore/montage.h"
#include "MagickCore/memory_.h"
#include "MagickCore/option.h"
#include "MagickCore/pixel-accessor.h"
#include "MagickCore/quantum-private.h"
#include "MagickCore/static.h"
#include "MagickCore/string_.h"
#include "MagickCore/string-private.h"
#include "MagickCore/module.h"
#include "MagickCore/utility.h"
#include "MagickCore/xwindow.h"
#include "MagickCore/xwindow-private.h"
#if defined(MAGICKCORE_HEIC_DELEGATE)
#include <libde265/de265.h>
#endif

/*
  Forward declarations.
*/
#if defined(MAGICKCORE_HEIC_DELEGATE)
/*
static MagickBooleanType
  WriteWEBPImage(const ImageInfo *,Image *);
*/
#endif

#if defined(MAGICKCORE_HEIC_DELEGATE)

#define MAX_ASSOCS_COUNT 10
#define MAX_ITEM_PROPS 100
#define MAX_HVCC_ATOM_SIZE 1024
#define MAX_ATOMS_IN_BOX 100
#define BUFFER_SIZE 100

typedef struct _HEICItemInfo
{
  unsigned int
    type;

  unsigned int
    assocsCount;

  uint8_t
    assocs[MAX_ASSOCS_COUNT];

  unsigned int
    dataSource;

  unsigned int
    offset;

  unsigned int
    size;
} HEICItemInfo;

typedef struct _HEICItemProp
{
  unsigned int
    type;

  unsigned int
    size;

  uint8_t
    *data;
} HEICItemProp;

typedef struct _HEICGrid
{
  unsigned int
    rowsMinusOne;

  unsigned int
    columnsMinusOne;

  unsigned int
    imageWidth;

  unsigned int
    imageHeight;
} HEICGrid;

typedef struct _HEICImageContext
{
  MagickBooleanType
    finished;

  int
    idsCount;

  HEICItemInfo
    *itemInfo;

  int
    itemPropsCount;

  HEICItemProp
    itemProps[MAX_ITEM_PROPS];

  unsigned int
    idatSize;

  uint8_t
    *idat;

  HEICGrid
    grid;

  de265_decoder_context
    *h265Ctx;

  unsigned int exifSize;
  uint8_t *exif;
  
  Image
    *tmp;
} HEICImageContext;

#define ATOM(a,b,c,d) ((a << 24) + (b << 16) + (c << 8) + d)
#define ThrowAndReturn(msg) ThrowFileException(exception, CorruptImageError, "Bad image: " # msg, __func__);

inline static char* intToAtom(unsigned int data)
{
  static char atom[5];

  atom[0] = (data >> 24) & 0xff;
  atom[1] = (data >> 16) & 0xff;
  atom[2] = (data >>  8) & 0xff;
  atom[3] = (data      ) & 0xff;
  atom[4] = '\0';

  return atom;
}

/*
   %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
   %                                                                             %
   %                                                                             %
   %                                                                             %
   %   I s H E I C                                                               %
   %                                                                             %
   %                                                                             %
   %                                                                             %
   %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
   %
   %  IsHEIC() returns MagickTrue if the image format type, identified by the
   %  magick string, is Heic.
   %
   %  The format of the IsHEIC method is:
   %
   %      MagickBooleanType IsHEIC(const unsigned char *magick,const size_t length)
   %
   %  A description of each parameter follows:
   %
   %    o magick: compare image format pattern against these bytes.
   %
   %    o length: Specifies the length of the magick string.
   %
   */
static MagickBooleanType IsHEIC(const unsigned char *magick,const size_t length)
{
  if (length < 12)
    return(MagickFalse);
  if (LocaleNCompare((const char *) magick+8,"heic",4) == 0)
    return(MagickTrue);
  return(MagickFalse);
}

static MagickSizeType ParseAtom(Image *image, MagickSizeType size,
    HEICImageContext *ctx, ExceptionInfo *exception);

static MagickBooleanType ParseFullBox(Image *image, MagickSizeType size,
    unsigned int atom, HEICImageContext *ctx, ExceptionInfo *exception)
{
  unsigned int
    version, flags, i;

  flags = ReadBlobMSBLong(image);
  version = flags >> 24;
  flags &= 0xffffff;

  (void) flags;
  (void) version;

  if (size < 4) {
    ThrowAndReturn("atom is too short");
  }

  size -= 4;

  for (i = 0; i < MAX_ATOMS_IN_BOX && size > 0; i++) {
    size = ParseAtom(image, size, ctx, exception);
  }

  return MagickTrue;
}

static MagickBooleanType ParseBox(Image *image, MagickSizeType size,
    unsigned int atom, HEICImageContext *ctx, ExceptionInfo *exception)
{
  unsigned int
    i;

  for (i = 0; i < MAX_ATOMS_IN_BOX && size > 0; i++) {
    size = ParseAtom(image, size, ctx, exception);
  }

  return MagickTrue;
}

static MagickBooleanType ParseHvcCAtom(HEICItemProp *prop, ExceptionInfo *exception)
{
  size_t
    size, pos, count, i;

  uint8_t
    buffer[MAX_HVCC_ATOM_SIZE];

  uint8_t
    *p;

  p = prop->data;

  size = prop->size;
  memcpy(buffer, prop->data, size);

  pos = 22;
  if (pos >= size) {
    ThrowAndReturn("hvcC atom is too short");
  }

  count = buffer[pos++];

  for (i = 0; i < count && pos < size-3; i++) {
    size_t
      naluType, num, j;

    naluType = buffer[pos++] & 0x3f;
    (void) naluType;
    num = buffer[pos++] << 8;
    num += buffer[pos++];

    for (j = 0; j < num && pos < size-2; j++) {
      size_t
        naluSize;

      naluSize = buffer[pos++] << 8;
      naluSize += buffer[pos++];

      if ((pos + naluSize > size) ||
          (p + naluSize > prop->data + prop->size)) {
        ThrowAndReturn("hvcC atom is too short");
      }

      // AnnexB NALU header
      *p++ = 0;
      *p++ = 0;
      *p++ = 0;
      *p++ = 1;

      memcpy(p, buffer + pos, naluSize);
      p += naluSize;
      pos += naluSize;
    }
  }

  prop->size = p - prop->data;
  return MagickTrue;
}

static MagickBooleanType ParseIpcoAtom(Image *image, MagickSizeType size,
    HEICImageContext *ctx, ExceptionInfo *exception)
{
  unsigned int
    length, atom;
  HEICItemProp
    *prop;
  ssize_t
    count;


  /*
     property indicies starts from 1
     */
  for (ctx->itemPropsCount = 1; ctx->itemPropsCount < MAX_ITEM_PROPS && size > 8; ctx->itemPropsCount++) {

    length = ReadBlobMSBLong(image);
    atom = ReadBlobMSBLong(image);

    if (ctx->itemPropsCount == MAX_ITEM_PROPS) {
      ThrowAndReturn("too many item properties");
    }

    prop = &(ctx->itemProps[ctx->itemPropsCount]);
    prop->type = atom;
    prop->size = length - 8;
    prop->data = AcquireMagickMemory(prop->size);
    count = ReadBlob(image, prop->size, prop->data);
    if (count != prop->size) {
      ThrowAndReturn("incorrect read size");
    }

    switch (prop->type) {
      case ATOM('h', 'v', 'c', 'C'):
        ParseHvcCAtom(prop, exception);
        break;
    }

    size -= length;
  }

  if (size > 0) {
    DiscardBlobBytes(image, size);
  }

  return MagickTrue;
}

static MagickBooleanType ParseIinfAtom(Image *image, MagickSizeType size,
    HEICImageContext *ctx, ExceptionInfo *exception)
{
  unsigned int
    version, flags, count, i;

  if (size < 4) {
    ThrowAndReturn("atom is too short");
  }

  flags = ReadBlobMSBLong(image);
  version = flags >> 24;
  flags = 0xffffff;

  size -= 4;

  if (version == 0) {
    count = ReadBlobMSBShort(image);
    size -= 2;
  } else {
    count = ReadBlobMSBLong(image);
    size -= 4;
  }

  /*
     item indicies starts from 1
     */
  ctx->idsCount = count;
  ctx->itemInfo = (HEICItemInfo *)AcquireMagickMemory(sizeof(HEICItemInfo)*(count+1));
  if (ctx->itemInfo == (HEICItemInfo *) NULL)
  {
    ThrowAndReturn("unable to allocate memory");
  }

  ResetMagickMemory(ctx->itemInfo, 0, sizeof(HEICItemInfo)*(count+1));

  for (i = 0; i < count && size > 0; i++)
  {
    size = ParseAtom(image, size, ctx, exception);
  }

  if (size > 0) {
    DiscardBlobBytes(image, size);
  }

  return MagickTrue;
}

static MagickBooleanType ParseInfeAtom(Image *image, MagickSizeType size,
    HEICImageContext *ctx, ExceptionInfo *exception)
{
  unsigned int
    version, flags, id, type;

  if (size < 9) {
    ThrowAndReturn("atom is too short");
  }

  flags = ReadBlobMSBLong(image);
  version = flags >> 24;
  flags = 0xffffff;

  size -= 4;

  if (version != 2) {
    ThrowAndReturn("unsupported infe atom version");
  }

  id = ReadBlobMSBShort(image);
  DiscardBlobBytes(image, 2); // item protection index
  type = ReadBlobMSBLong(image);
  size -= 8;

  /*
     item indicies starts from 1
     */
  if (id > ctx->idsCount) {
    ThrowAndReturn("item id is incorrect");
  }

  ctx->itemInfo[id].type = type;

  if (size > 0) {
    DiscardBlobBytes(image, size);
  }

  return MagickTrue;
}

static MagickBooleanType ParseIpmaAtom(Image *image, MagickSizeType size,
    HEICImageContext *ctx, ExceptionInfo *exception)
{
  unsigned int
    version, flags, count, i;

  if (size < 9) {
    ThrowAndReturn("atom is too short");
  }

  flags = ReadBlobMSBLong(image);
  version = flags >> 24;
  flags = 0xffffff;

  size -= 4;

  count = ReadBlobMSBLong(image);
  size -= 4;

  for (i = 0; i < count && size > 2; i++) {
    unsigned int
      id, assoc_count, j;

    if (version < 1) {
      id = ReadBlobMSBShort(image);
      size -= 2;
    } else {
      id = ReadBlobMSBLong(image);
      size -= 4;
    }

    /*
       item indicies starts from 1
       */
    if (id > ctx->idsCount) {
      ThrowAndReturn("item id is incorrect");
    }

    assoc_count = ReadBlobByte(image);
    size -= 1;

    if (assoc_count > MAX_ASSOCS_COUNT) {
      ThrowAndReturn("too many associations");
    }

    for (j = 0; j < assoc_count && size > 0; j++) {
      ctx->itemInfo[id].assocs[j] = ReadBlobByte(image);
      size -= 1;
    }

    ctx->itemInfo[id].assocsCount = j;
  }

  if (size > 0) {
    DiscardBlobBytes(image, size);
  }

  return MagickTrue;
}

static MagickBooleanType ParseIlocAtom(Image *image, MagickSizeType size,
    HEICImageContext *ctx, ExceptionInfo *exception)
{
  unsigned int
    version, flags, tmp, count, i;

  if (size < 9) {
    ThrowAndReturn("atom is too short");
  }

  flags = ReadBlobMSBLong(image);
  version = flags >> 24;
  flags = 0xffffff;

  size -= 4;

  tmp = ReadBlobByte(image);
  if (tmp != 0x44) {
    ThrowAndReturn("only offset_size=4 and length_size=4 are supported");
  }
  tmp = ReadBlobByte(image);
  if (tmp != 0x00) {
    ThrowAndReturn("only base_offset_size=0 and index_size=0 are supported");
  }
  size -= 2;

  if (version < 2) {
    count = ReadBlobMSBShort(image);
    size -= 2;
  } else {
    count = ReadBlobMSBLong(image);
    size -= 4;
  }

  for (i = 0; i < count && size > 2; i++) {
    unsigned int
      id, ext_count;

    HEICItemInfo
      *item;

    id = ReadBlobMSBShort(image);
    size -= 2;

    /*
       item indicies starts from 1
       */
    if (id > ctx->idsCount) {
      ThrowAndReturn("item id is incorrect");
    }

    item = &ctx->itemInfo[id];

    if (version == 1 || version == 2) {
      item->dataSource = ReadBlobMSBShort(image);
      size -= 2;
    }

    /*
     * data ref index
     */
    DiscardBlobBytes(image, 2);
    size -= 2;

    ext_count = ReadBlobMSBShort(image);
    size -= 2;

    if (ext_count != 1) {
      ThrowAndReturn("only one excention per item is supported");
    }

    item->offset = ReadBlobMSBLong(image);
    item->size = ReadBlobMSBLong(image);
    size -= 8;
  }

  if (size > 0) {
    DiscardBlobBytes(image, size);
  }

  return MagickTrue;
}

static MagickSizeType ParseAtom(Image *image, MagickSizeType size,
    HEICImageContext *ctx, ExceptionInfo *exception)
{
  MagickBooleanType
    status;

  MagickSizeType
    atom_size;

  unsigned int
    atom;

  if (size < 8)
  {
    ThrowAndReturn("atom is too short");
  }

  atom_size = ReadBlobMSBLong(image);
  atom = ReadBlobMSBLong(image);

  if (atom_size == 1) {
    ReadBlobMSBLong(image);
    atom_size = ReadBlobMSBLong(image);
  }

  if (atom_size > size)
  {
    ThrowAndReturn("atom is too short");
  }
  
  status = MagickTrue;

  switch (atom)
  {
    case ATOM('f', 't', 'y', 'p'):
      DiscardBlobBytes(image, atom_size-8);
      break;
    case ATOM('m', 'e', 't', 'a'):
    case ATOM('i', 'r', 'e', 'f'):
      status = ParseFullBox(image, atom_size - 8, atom, ctx, exception);
      break;
    case ATOM('i', 'p', 'r', 'p'):
      status = ParseBox(image, atom_size - 8, atom, ctx, exception);
      break;
    case ATOM('i', 'i', 'n', 'f'):
      status = ParseIinfAtom(image, atom_size - 8, ctx, exception);
      break;
    case ATOM('i', 'n', 'f', 'e'):
      status = ParseInfeAtom(image, atom_size - 8, ctx, exception);
      break;
    case ATOM('i', 'p', 'c', 'o'):
      status = ParseIpcoAtom(image, atom_size - 8, ctx, exception);
      break;
    case ATOM('i', 'p', 'm', 'a'):
      status = ParseIpmaAtom(image, atom_size - 8, ctx, exception);
      break;
    case ATOM('i', 'l', 'o', 'c'):
      status = ParseIlocAtom(image, atom_size - 8, ctx, exception);
      break;
    case ATOM('i', 'd', 'a', 't'):
      {
        ssize_t
          count;
        ctx->idatSize = atom_size - 8;
        ctx->idat = AcquireMagickMemory(ctx->idatSize);
        if (ctx->idat == NULL) {
          ThrowAndReturn("unable to allocate memory");
        }

        count = ReadBlob(image, ctx->idatSize, ctx->idat);
        if (count != ctx->idatSize) {
          ThrowAndReturn("unable to read idat");
        }
      }
      break;
    case ATOM('m', 'd', 'a', 't'):
      ctx->finished = MagickTrue;
      break;
    default:
      //printf("skipping unknown atom %s with size %u\n", intToAtom(atom), atom_size);
      DiscardBlobBytes(image, atom_size-8);
      break;
  }

  if (status != MagickTrue)
    ThrowAndReturn("atom parsing failed");

  return size - atom_size;
}

static MagickBooleanType decodeGrid(HEICImageContext *ctx, ExceptionInfo *exception)
{
  unsigned int
    i, flags;

  for (i = 1; i < ctx->idsCount; i++) {
    HEICItemInfo
      *info = &ctx->itemInfo[i];
    if (info->type != ATOM('g','r','i','d'))
      continue;
    if (info->dataSource != 1) {
      ThrowAndReturn("unsupport data source type");
    }

    if (ctx->idatSize < 8) {
      ThrowAndReturn("idat is too small");
    }

    flags = ctx->idat[1];

    ctx->grid.rowsMinusOne = ctx->idat[2];
    ctx->grid.columnsMinusOne = ctx->idat[3];

    if (flags & 1) {
      ThrowAndReturn("Only 16 bits sizes are supported");
    }

    ctx->grid.imageWidth = (ctx->idat[4] << 8) + ctx->idat[5];
    ctx->grid.imageHeight = (ctx->idat[6] << 8) + ctx->idat[7];

    return MagickTrue;
  }
  return MagickFalse;
}

static MagickBooleanType decodeH265Image(Image *image, HEICImageContext *ctx, unsigned int id, ExceptionInfo *exception)
{
  unsigned char
    *buffer = NULL;

  size_t
    count, pos;

  int
    more, i;

  unsigned int
    x_offset, y_offset;

  de265_error
    err;

  pos = 0;
  de265_reset(ctx->h265Ctx);

  x_offset = 512 * ((id-1) % (ctx->grid.columnsMinusOne + 1));
  y_offset = 512 * ((id-1) / (ctx->grid.columnsMinusOne + 1));

  for (i = 0; i < ctx->itemInfo[id].assocsCount; i++) {
    size_t
      assoc;

    assoc = ctx->itemInfo[id].assocs[i] & 0x7f;
    if (assoc > ctx->itemPropsCount) {
      ThrowFileException(exception, CorruptImageError,"Bad image: incorrect item property index", "decodeH265Image");
      goto err_out_free;
    }

    if (ctx->itemProps[assoc].type != ATOM('h', 'v', 'c', 'C')) {
      //TODO work with other property types
      continue;
    }

    err = de265_push_data(ctx->h265Ctx, ctx->itemProps[assoc].data, ctx->itemProps[assoc].size, pos, (void*)2);
    if (err != DE265_OK) {
      ThrowFileException(exception, CorruptImageError,"Bad image: unable to push data", "decodeH265Image");
      goto err_out_free;
    }

    pos += ctx->itemProps[assoc].size;
  }

  buffer = AcquireMagickMemory(ctx->itemInfo[id].size);
  if (buffer == NULL) {
    ThrowFileException(exception, CorruptImageError,"Bad image: unable to allocate memory", "decodeH265Image");
    return MagickFalse;
  }

  SeekBlob(image, ctx->itemInfo[id].offset, SEEK_SET);
  count = ReadBlob(image, ctx->itemInfo[id].size, buffer);
  if (count != ctx->itemInfo[id].size) {
    ThrowFileException(exception, CorruptImageError,"Bad image: unable to read data", "decodeH265Image");
    goto err_out_free;
  }

  /*
   * AVCC to AnnexB
   */
  buffer[0] = 0;
  buffer[1] = 0;
  buffer[2] = 0;
  buffer[3] = 1;

  err = de265_push_data(ctx->h265Ctx, buffer, ctx->itemInfo[id].size, pos, (void*)2);
  if (err != DE265_OK) {
    ThrowFileException(exception, CorruptImageError,"Bad image: unable to push data", "decodeH265Image");
    goto err_out_free;
  }

  err = de265_flush_data(ctx->h265Ctx);
  if (err != DE265_OK) {
    ThrowFileException(exception, CorruptImageError,"Bad image: unable to push data", "decodeH265Image");
    goto err_out_free;
  }

  more = 0;

  do {
    err = de265_decode(ctx->h265Ctx, &more);
    if (err != DE265_OK) {
      ThrowFileException(exception, CorruptImageError,"Bad image: unable to decode data", "decodeH265Image");
      goto err_out_free;
    }

    while (1) {
      de265_error warning = de265_get_warning(ctx->h265Ctx);
      if (warning==DE265_OK) {
        break;
      }

      ThrowMagickException(exception, GetMagickModule(), CoderWarning, "Warning: decoding image: ", "%s", de265_get_error_text(warning));
    }

    const struct de265_image* img = de265_get_next_picture(ctx->h265Ctx);
    if (img) {
      const uint8_t *planes[3];
      int dims[3][2];
      int strides[3];

      int c;
      for (c = 0; c < 3; c++) {
        planes[c] = de265_get_image_plane(img, c, &(strides[c]));
        dims[c][0] = de265_get_image_width(img, c);
        dims[c][1] = de265_get_image_height(img, c);
      }


      assert(dims[0][0] == 512);
      assert(dims[0][1] == 512);
      assert(dims[1][0] == 256);
      assert(dims[1][1] == 256);
      assert(dims[2][0] == 256);
      assert(dims[2][1] == 256);

      Image* chroma;

      chroma = ctx->tmp;

      int x, y;

      for (y = 0; y < 256; y++) {
        register Quantum *q;
        register const uint8_t *p1 = planes[1] + y * strides[1];
        register const uint8_t *p2 = planes[2] + y * strides[2];

        q = QueueAuthenticPixels(chroma, 0, y, 256, 1, exception);
        if (q == NULL) {
          goto err_out_free;
        }

        for (x = 0; x < 256; x++) {
          SetPixelGreen(chroma, ScaleCharToQuantum(*p1++), q);
          SetPixelBlue(chroma, ScaleCharToQuantum(*p2++), q);
          q+=GetPixelChannels(chroma);
        }

        if (SyncAuthenticPixels(chroma, exception) == MagickFalse) {
          goto err_out_free;
        }
      }

      Image* resized_chroma = ResizeImage(chroma, 512, 512, TriangleFilter, exception);
      if (resized_chroma == NULL) {
        goto err_out_free;
      }

      for (y = 0; y < 512; y++) {
        register Quantum *q;
        register const Quantum *p;
        register const uint8_t *l = planes[0] + y * strides[0];

        q = QueueAuthenticPixels(image, x_offset, y_offset + y, 512, 1, exception);
        if (q == NULL) {
          goto err_loop_free;
        }

        p = GetVirtualPixels(resized_chroma, 0, y, 512, 1, exception);
        if (p == NULL) {
          goto err_loop_free;
        }

        for (x = 0; x < 512; x++) {
          SetPixelRed(image, ScaleCharToQuantum(*l), q);
          SetPixelGreen(image, GetPixelGreen(resized_chroma, p), q);
          SetPixelBlue(image, GetPixelBlue(resized_chroma, p), q);
          l++;
          q+=GetPixelChannels(image);
          p+=GetPixelChannels(resized_chroma);
        }

        if (SyncAuthenticPixels(image, exception) == MagickFalse) {
          goto err_loop_free;
        }
      }

      if (resized_chroma)
        resized_chroma = DestroyImage(resized_chroma);

      more = 0;
      break;

err_loop_free:
      if (resized_chroma)
        resized_chroma = DestroyImage(resized_chroma);

      goto err_out_free;
    }
  } while (more);

  return MagickTrue;

err_out_free:
  ThrowFileException(exception, CorruptImageError,"Bad image: error decoding h265", __func__);
  RelinquishMagickMemory(buffer);
  return MagickFalse;
}

/*
   %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
   %                                                                             %
   %                                                                             %
   %                                                                             %
   %   R e a d H E I C I m a g e                                                 %
   %                                                                             %
   %                                                                             %
   %                                                                             %
   %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
   %
   %  ReadWEBPImage() reads an image in the WebP image format.
   %
   %  The format of the ReadWEBPImage method is:
   %
   %      Image *ReadWEBPImage(const ImageInfo *image_info,
   %        ExceptionInfo *exception)
   %
   %  A description of each parameter follows:
   %
   %    o image_info: the image info.
   %
   %    o exception: return any errors or warnings in this structure.
   %
   */

static Image *ReadHEICImage(const ImageInfo *image_info,
    ExceptionInfo *exception)
{
  Image
    *image;

  MagickBooleanType
    status;

  //char
  //    buffer[BUFFER_SIZE];

  MagickSizeType
    length;

  ssize_t
    count,
    i;

  HEICImageContext
    ctx;

  /*
     Open image file.
     */
  assert(image_info != (const ImageInfo *) NULL);
  assert(image_info->signature == MagickCoreSignature);
  if (image_info->debug != MagickFalse)
    (void) LogMagickEvent(TraceEvent,GetMagickModule(),"%s",
        image_info->filename);
  assert(exception != (ExceptionInfo *) NULL);
  assert(exception->signature == MagickCoreSignature);
  image=AcquireImage(image_info,exception);
  status=OpenBlob(image_info,image,ReadBinaryBlobMode,exception);
  if (status == MagickFalse)
  {
    image=DestroyImageList(image);
    return((Image *) NULL);
  }
  if (!IsBlobSeekable(image))
    ThrowReaderException(CorruptImageError,"Only seekable sources are supported");

  ResetMagickMemory(&ctx, 0, sizeof(ctx));

  length=GetBlobSize(image);
  count = MAX_ATOMS_IN_BOX;
  while (length && ctx.finished == MagickFalse && count--)
  {
    length = ParseAtom(image, length, &ctx, exception);
    if (length == (MagickSizeType)-1) {
      ThrowReaderException(CorruptImageError,"Unable To Decode Image File");
    }
  }

  if (ctx.finished != MagickTrue) {
    ThrowReaderException(CorruptImageError,"Unable To Decode Image File");
  }

  /* find exif if present */
  ctx.exif = NULL;
  ctx.exifSize = 0;
  for(int i = 0; i <= ctx.idsCount; i++) {
    if(ctx.itemInfo[i].type == ATOM('E', 'x', 'i', 'f')) {
      ssize_t count;
      ctx.exifSize = ctx.itemInfo[i].size;
      ctx.exif = AcquireMagickMemory(ctx.exifSize);
      SeekBlob(image, ctx.itemInfo[i].offset, SEEK_SET);
      count = ReadBlob(image, ctx.exifSize, ctx.exif);
      if (count != ctx.exifSize) {
        ThrowReaderException(CorruptImageError,"Unable To Find Exif Data");
      }
      break;
    }
  }

  if(ctx.exif != NULL) {
    StringInfo
      *profile;
    
    profile = BlobToStringInfo((const void *)ctx.exif, ctx.exifSize);
    if (profile == (StringInfo *) NULL) {
      return NULL;
    }
    
    (void) SetImageProfile(image, "exif", profile, exception);
    
    profile = DestroyStringInfo(profile);
  }    
  
  /*
     Initialize h265 decoder
     */
  ctx.h265Ctx = de265_new_decoder();
  if (ctx.h265Ctx == NULL) {
    ThrowReaderException(CorruptImageError,"Unable To Initialize Decoder");
  }

  if (decodeGrid(&ctx, exception) != MagickTrue) {
    ThrowReaderException(CorruptImageError,"Unable to decode image grid");
  }

  count = (ctx.grid.rowsMinusOne + 1) * (ctx.grid.columnsMinusOne + 1);

  image->columns = 512 * (ctx.grid.columnsMinusOne + 1);
  image->rows = 512 * (ctx.grid.rowsMinusOne + 1);
  image->depth=8;

  ctx.tmp = CloneImage(image, 256, 256, MagickTrue, exception);
  if (ctx.tmp == NULL) {
    ThrowReaderException(CorruptImageError,"Unable to clone image");
  }

  DuplicateBlob(ctx.tmp, image);

  for (i = 0; i < count; i++) {
    decodeH265Image(image, &ctx, i+1, exception);
  }

  SetImageColorspace(image, YCbCrColorspace, exception);

  return image;
}
#endif

/*
   %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
   %                                                                             %
   %                                                                             %
   %                                                                             %
   %   R e g i s t e r H E I C I m a g e                                         %
   %                                                                             %
   %                                                                             %
   %                                                                             %
   %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
   %
   %  RegisterImage() adds attributes for the Heic image format to
   %  the list of supported formats.  The attributes include the image format
   %  tag, a method to read and/or write the format, whether the format
   %  supports the saving of more than one frame to the same file or blob,
   %  whether the format supports native in-memory I/O, and a brief
   %  description of the format.
   %
   %  The format of the RegisterHEICImage method is:
   %
   %      size_t RegisterHEICImage(void)
   %
   */
ModuleExport size_t RegisterHEICImage(void)
{
  MagickInfo
    *entry;

  entry=AcquireMagickInfo("HEIC", "HEIC", "Apple High efficiency Image Format");
#if defined(MAGICKCORE_HEIC_DELEGATE)
  entry->decoder=(DecodeImageHandler *) ReadHEICImage;
#endif
  entry->description=ConstantString("Heic Image Format");
  entry->mime_type=ConstantString("image/x-heic");
  entry->module=ConstantString("HEIC");
  entry->flags^=CoderAdjoinFlag;
  entry->magick=(IsImageFormatHandler *) IsHEIC;
  (void) RegisterMagickInfo(entry);
  return(MagickImageCoderSignature);
}

/*
   %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
   %                                                                             %
   %                                                                             %
   %                                                                             %
   %   U n r e g i s t e r H E I C I m a g e                                     %
   %                                                                             %
   %                                                                             %
   %                                                                             %
   %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
   %
   %  UnregisterHEICImage() removes format registrations made by the Heic module
   %  from the list of supported formats.
   %
   %  The format of the UnregisterHEICImage method is:
   %
   %      UnregisterHEICImage(void)
   %
   */
ModuleExport void UnregisterHEICImage(void)
{
  (void) UnregisterMagickInfo("HEIC");
}
