/*
jpeg2yuv
========

  Converts a collection of JPEG images to a YUV4MPEG stream.
  (see jpeg2yuv -h for help (or have a look at the function "usage"))
  
  Copyright (C) 1999 Gernot Ziegler (gz@lysator.liu.se)
  Copyright (C) 2001 Matthew Marjanovic (maddog@mir.com)

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include <string.h>
#include <errno.h>
#include <jpeglib.h>
#include "jpegutils.h"
#include "lav_io.h"

#include <sys/types.h>

#include "mjpeg_logging.h"
#include "mjpeg_types.h"

#include "yuv4mpeg.h"
#include "mpegconsts.h"

#define MAXPIXELS (1280*1024)  /* Maximum size of final image */
#define BUFFERSIZE 10000

/* strip string */
void strip(char *p) {
    while (1) {
        if ((*p == '\r') || (*p == '\n'))
            *p = '\0';
        if (*p == '\0')
            break;
        p++;
    }
}

typedef struct _parameters {
  char *jpegformatstr;
  uint32_t begin;       /* the video frame start */
  int32_t numframes;   /* -1 means: take all frames */
  y4m_ratio_t framerate;
  y4m_ratio_t aspect_ratio;
  int interlace;   /* will the YUV4MPEG stream be interlaced? */
  int interleave;  /* are the JPEG frames field-interleaved? */
  int verbose; /* the verbosity of the program (see mjpeg_logging.h) */

  int width;
  int height;
  int colorspace;
  int loop;
  int rescale_YUV;
} parameters_t;


typedef struct _read_parameters {
    ssize_t buff_size;
    uint8_t *frame_buff;
} read_parameters_t;


static struct jpeg_decompress_struct dinfo;
static struct jpeg_error_mgr jerr;


static char encoding_table[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
                                'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                                'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                                'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                                'w', 'x', 'y', 'z', '0', '1', '2', '3',
                                '4', '5', '6', '7', '8', '9', '+', '/' };
static char* decoding_table = NULL;
static int mod_table[] = { 0, 2, 1 };


/*
 * The User Interface parts 
 */

/* usage
 * Prints a short description of the program, including default values 
 * in: prog: The name of the program 
 */
static void usage(char *prog)
{
  char *h;
  
  if (NULL != (h = (char *)strrchr(prog,'/')))
    prog = h+1;
  
  fprintf(stderr, 
	  "usage: %s [ options ]\n"
	  "\n"
	  "where options are ([] shows the defaults):\n"
	  "  -l num        loop -1=forever, n >= 1 n-times       \n"
	  "  -v num        verbosity (0,1,2)                  [1]\n"
	  "  -b framenum   starting frame number              [0]\n"
	  "  -f framerate  framerate for output stream (fps)     \n"
          "  -A sar        output sample aspect ratio         [1:1]\n" 
	  "  -n numframes  number of frames to process        [-1 = all]\n"
	  "  -j {1}%%{2}d{3} Read JPEG frames with the name components as follows:\n"
	  "               {1} JPEG filename prefix (e g rendered_ )\n"
	  "               {2} Counting placeholder (like in C, printf, eg 06 ))\n"
	  "  -I x  interlacing mode:  p = none/progressive\n"
	  "                           t = top-field-first\n"
	  "                           b = bottom-field-first\n"
	  "  -L x  interleaving mode:  0 = non-interleaved (two successive\n"
	  "                                 fields per JPEG file)\n"
	  "                            1 = interleaved fields\n"
	  "  -R 1/0 ... 1: rescale YUV color values from 0-255 to 16-235 (default: 1)\n"
	  "\n"
	  "%s pipes a sequence of JPEG files to stdout,\n"
	  "making the direct encoding of MPEG files possible under mpeg2enc.\n"
	  "Any JPEG format supported by libjpeg can be read.\n"
	  "stdout will be filled with the YUV4MPEG movie data stream,\n"
	  "so be prepared to pipe it on to mpeg2enc or to write it into a file.\n"
	  "\n"
	  "If -j option is omited, filenames are read from stdin.\n"
	  "\n"
	  "examples:\n"
	  "  ls *jpg | %s -f 25 -I p > result.yuv\n"
	  "  | convert all jpg files in curent directory \n"
	  "  %s -j in_%%06d.jpeg -b 100000 > result.yuv\n"
	  "  | combines all the available JPEGs that match \n"
	  "    in_??????.jpeg, starting with 100000 (in_100000.jpeg, \n"
	  "    in_100001.jpeg, etc...) into the uncompressed YUV4MPEG videofile result.yuv\n"
	  "  %s -It -L0 -j abc_%%04d.jpeg | mpeg2enc -f3 -o out.m2v\n"
	  "  | combines all the available JPEGs that match \n"
	  "    abc_??????.jpeg, starting with 0000 (abc_0000.jpeg, \n"
	  "    abc_0001.jpeg, etc...) and pipes it to mpeg2enc which encodes\n"
	  "    an MPEG2-file called out.m2v out of it\n"
	  "\n",
	  prog, prog, prog, prog, prog);
}



/* parse_commandline
 * Parses the commandline for the supplied parameters.
 * in: argc, argv: the classic commandline parameters
 */
static void parse_commandline(int argc, char ** argv, parameters_t *param)
{
  int c, sts;
  
  param->jpegformatstr = NULL;
  param->begin = 0;
  param->numframes = -1;
  param->framerate = y4m_fps_UNKNOWN;
  param->interlace = Y4M_UNKNOWN;
  param->aspect_ratio = y4m_sar_SQUARE;
  param->interleave = -1;
  param->verbose = 1;
  param->loop = 1;
  param->rescale_YUV = 1;

  /* parse options */
  for (;;) {
    if (-1 == (c = getopt(argc, argv, "I:hv:L:b:j:n:f:l:R:A:")))
      break;
    switch (c) {

    case 'A':
      sts = y4m_parse_ratio(&param->aspect_ratio, optarg);
      if (sts != Y4M_OK)
         mjpeg_error_exit1("Invalid aspect ratio: %s", optarg);
      break;
    case 'j':
      param->jpegformatstr = strdup(optarg);
      break;
    case 'b':
      param->begin = atol(optarg);
      break;
    case 'n':
      param->numframes = atol(optarg);
      break;
    case 'R':
      param->rescale_YUV = atoi(optarg);
      break;
    case 'f':
      param->framerate = mpeg_conform_framerate(atof(optarg));
      break;
    case 'I':
      switch (optarg[0]) 
	{
	case 'p':
	  param->interlace = Y4M_ILACE_NONE;
	  break;
	case 't':
	  param->interlace = Y4M_ILACE_TOP_FIRST;
	  break;
	case 'b':
	  param->interlace = Y4M_ILACE_BOTTOM_FIRST;
	  break;
	default:
	  mjpeg_error_exit1 ("-I option requires arg p, t, or b");
	}
      break;
    case 'L':
      param->interleave = atoi(optarg);
      if ((param->interleave != 0) &&
	  (param->interleave != 1)) 
	mjpeg_error_exit1 ("-L option requires arg 0 or 1");
      break;
    case 'v':
      param->verbose = atoi(optarg);
      if (param->verbose < 0 || param->verbose > 2) 
	mjpeg_error_exit1( "-v option requires arg 0, 1, or 2");    
      break;     
    case 'l':
      param->loop = atoi(optarg);
      if  (param->loop == 0 || param->loop < -1 ) 
	mjpeg_error_exit1( "-l option requires a number greater than 0 or -1 to loop forever ");    
      break;     
    case 'h':
    default:
      mjpeg_info("Wp x, char %c\n", c);

      usage(argv[0]);
      exit(1);
    }
  }

  param->numframes = 1;

  if (param->jpegformatstr == NULL)
      mjpeg_info("Reading jpeg filenames from stdin.");

  if (Y4M_RATIO_EQL(param->framerate, y4m_fps_UNKNOWN)) {
    mjpeg_error("%s:  framerate not specified.  (Use -f option)",
		argv[0]); 
    usage(argv[0]); 
    exit(1);
  }
}


/*
 * The file handling parts 
 */

/** init_parse_files
 * Verifies the JPEG input files and prepares YUV4MPEG header information.
 * remember first filename for later
 * @returns 0 on success
 */
static int init_parse_files(parameters_t *param, uint8_t* jpegdata, ssize_t jpegsize)
{ 
  mjpeg_info("Parsing & checking input files.");
  mjpeg_debug("Analyzing image to get the right pic params");

  /* Now open this JPEG file, and examine its header to retrieve the 
     YUV4MPEG info that shall be written */
  dinfo.err = jpeg_std_error(&jerr);  /* ?????????? */
  jpeg_create_decompress(&dinfo);
  jpeg_buffer_src(&dinfo, jpegdata, jpegsize);
  jpeg_read_header(&dinfo, TRUE);
  switch (dinfo.jpeg_color_space)
    {
    case JCS_YCbCr:
      mjpeg_info("YUV colorspace detected.\n"); 
      dinfo.out_color_space = JCS_YCbCr;      
      break;
    case JCS_GRAYSCALE:
      mjpeg_info("Grayscale colorspace detected.\n"); 
      dinfo.out_color_space = JCS_GRAYSCALE;      
      break;
    default:
      mjpeg_error("Unsupported colorspace detected.\n"); break;
    }

  mjpeg_info("Starting decompression");

  jpeg_start_decompress(&dinfo);
  
  if (dinfo.output_components != 3 && dinfo.out_color_space == JCS_YCbCr)
    mjpeg_error_exit1("Output components of color JPEG image = %d, must be 3.",
  		      dinfo.output_components);

  if (dinfo.output_components != 1 && dinfo.out_color_space == JCS_GRAYSCALE)
    mjpeg_error_exit1("Output components of grayscale JPEG image = %d, must be 1.",
  		      dinfo.output_components);
  
  mjpeg_info("Image dimensions are %dx%d",
	     dinfo.image_width, dinfo.image_height);
  /* picture size check  */
  if ( (dinfo.image_width % 16) != 0 )
    mjpeg_error_exit1("The image width isn't a multiple of 16, rescale the image");
  if ( (dinfo.image_height % 16) != 0 )
    mjpeg_error_exit1("The image height isn't a multiple of 16, rescale the image");

  param->width = dinfo.image_width;
  param->height = dinfo.image_height;
  param->colorspace = dinfo.jpeg_color_space;
  
  jpeg_destroy_decompress(&dinfo);

  mjpeg_info("Movie frame rate is:  %f frames/second",
	     Y4M_RATIO_DBL(param->framerate));

  switch (param->interlace) {
  case Y4M_ILACE_NONE:
    mjpeg_info("Non-interlaced/progressive frames.");
    break;
  case Y4M_ILACE_BOTTOM_FIRST:
    mjpeg_info("Interlaced frames, bottom field first.");      
    break;
  case Y4M_ILACE_TOP_FIRST:
    mjpeg_info("Interlaced frames, top field first.");      
    break;
  default:
    mjpeg_error_exit1("Interlace has not been specified (use -I option)");
    break;
  }

  if ((param->interlace != Y4M_ILACE_NONE) && (param->interleave == -1))
    mjpeg_error_exit1("Interleave has not been specified (use -L option)");

  if (!(param->interleave) && (param->interlace != Y4M_ILACE_NONE)) {
    param->height *= 2;
    mjpeg_info("Non-interleaved fields (image height doubled)");
  }
  mjpeg_info("Frame size:  %d x %d", param->width, param->height);

  return 0;
}

/**
  Rescales the YUV values from the range 0..255 to the range 16..235 
  @param yp: buffer for Y plane of decoded JPEG 
  @param up: buffer for U plane of decoded JPEG 
  @param vp: buffer for V plane of decoded JPEG 
*/
static void rescale_color_vals(int width, int height, uint8_t *yp, uint8_t *up, uint8_t *vp) 
{
  int x,y;
  for (y = 0; y < height; y++)
    for (x = 0; x < width; x++)
      yp[x+y*width] = (float)(yp[x+y*width]) * ((235.0 - 16.0)/255.0) + 16.0;

  for (y = 0; y < height/2; y++)
    for (x = 0; x < width/2; x++)
      {
	up[x+y*width/2] = (float)(up[x+y*width/2]) * ((240.0 - 16.0)/255.0) + 16.0;
	vp[x+y*width/2] = (float)(vp[x+y*width/2]) * ((240.0 - 16.0)/255.0) + 16.0;
      }
}

/**
  Open and read a file if the file name
  is not the same as the previous file name.
  @param jpegdata: buffer where the JPEG data will be read into
  @param jpegname: JPEG file name
  @param prev_jpegname: previous JPEG file name
  @returns 0  if the previous read data is still valid.
           -1 if the file could not be opened.
           >0 the number of bytes read into jpegdata.
*/
static ssize_t read_jpeg_data(uint8_t *jpegdata, char *jpegname, char *prev_jpegname)
{
  FILE *jpegfile;
  ssize_t jpegsize;
  if (strncmp(jpegname, prev_jpegname, strlen(jpegname)) != 0) {
    strncpy(prev_jpegname, jpegname, strlen(jpegname));
    jpegfile = fopen(jpegname, "rb");
    if (jpegfile == NULL) { 
      jpegsize = -1;
      mjpeg_info("Read from '%s' failed:  %s", jpegname, strerror(errno));
    } else {
      jpegsize = fread(jpegdata, sizeof(unsigned char), MAXPIXELS, jpegfile); 
      fclose(jpegfile);
    }
  }
  else {
    jpegsize = 0;
  }
  return jpegsize;
}


static ssize_t find_eoi_in_buffer(uint8_t* buffer, ssize_t size) {
    ssize_t i;
    for (i = 0; i < size - 1; i++) {
        if (buffer[i] == 0xFF && buffer[i + 1] == 0xD9) {
            mjpeg_debug("EOI index: %d", i + 1);
            return i + 1;
        }
    }
    mjpeg_debug("Failed to found EOI");
    return -1;
}

static ssize_t read_from_stdin(read_parameters_t* read_params) {
    ssize_t read_size = 0, eoi_index = -1;
    uint8_t buffer[BUFFERSIZE];

    while (eoi_index == -1) {
        mjpeg_debug("Reading to buffer...");
        read_size = fread(buffer, sizeof(unsigned char), BUFFERSIZE, stdin);
        mjpeg_debug("Read size: %d", read_size);
        if (read_size <= 0) {
            mjpeg_debug("Failed to read from STDIN! Size: %d", read_size);
            return read_size;
        }
        eoi_index = find_eoi_in_buffer(buffer, read_size);

        memcpy(read_params->frame_buff + read_params->buff_size, buffer, read_size * sizeof(unsigned char));
        read_params->buff_size += read_size;
        mjpeg_debug("Bufer size: %d", read_params->buff_size);
    }
    return read_params->buff_size - read_size + (eoi_index + 1);
}


static ssize_t read_jpeg_from_stdin(uint8_t* jpegdata, read_parameters_t *read_params) {
    ssize_t image_size;

    if (read_params->buff_size == 0 || find_eoi_in_buffer(read_params->frame_buff, read_params->buff_size) == -1) {
        // Frame buffer is empty, or partial image is available in the buffer
        image_size = read_from_stdin(read_params);
        mjpeg_debug("Image size: %d", image_size);
        if (image_size <= 0) {
            return image_size;
        }

        memcpy(jpegdata, read_params->frame_buff, image_size * sizeof(unsigned char));
        if (read_params->buff_size - image_size != 0) {
            memmove(read_params->frame_buff,
                        read_params->frame_buff + image_size,
                            (read_params->buff_size - image_size) * sizeof(unsigned char));
        }
        read_params->buff_size -= image_size;
        return image_size;
    }
    else {
        // A whole image in the buffer is waiting...
        ssize_t new_buff_size, eoi_index;
        eoi_index = find_eoi_in_buffer(read_params->frame_buff, read_params->buff_size);

        memcpy(jpegdata, read_params->frame_buff, (eoi_index + 1) * sizeof(unsigned char));
        new_buff_size = read_params->buff_size - (eoi_index + 1);
        if (new_buff_size != 0) {
            mjpeg_debug("Should move the remaining part...");
            memmove(read_params->frame_buff, 
                        read_params->frame_buff + eoi_index + 1, 
                            new_buff_size * sizeof(unsigned char));
        }
        else {
            mjpeg_debug("Frame buffer is empty...");
        }
        read_params->buff_size = new_buff_size;

        return eoi_index + 1;
    }
}

// base64 functions copied from: https://stackoverflow.com/a/6782480/8044326
static void build_decoding_table() {

    decoding_table = malloc(256);

    for (int i = 0; i < 64; i++)
        decoding_table[(unsigned char)encoding_table[i]] = i;
}


static void base64_cleanup() {
    free(decoding_table);
}


static unsigned char* base64_decode(const char* data, size_t input_length, size_t* output_length) {

    if (decoding_table == NULL) build_decoding_table();

    if (input_length % 4 != 0) return NULL;

    *output_length = input_length / 4 * 3;
    if (data[input_length - 1] == '=') (*output_length)--;
    if (data[input_length - 2] == '=') (*output_length)--;

    unsigned char* decoded_data = malloc(*output_length);
    if (decoded_data == NULL) return NULL;

    for (int i = 0, j = 0; i < input_length;) {

        uint32_t sextet_a = data[i] == '=' ? 0 & i++ : decoding_table[data[i++]];
        uint32_t sextet_b = data[i] == '=' ? 0 & i++ : decoding_table[data[i++]];
        uint32_t sextet_c = data[i] == '=' ? 0 & i++ : decoding_table[data[i++]];
        uint32_t sextet_d = data[i] == '=' ? 0 & i++ : decoding_table[data[i++]];

        uint32_t triple = (sextet_a << 3 * 6)
            + (sextet_b << 2 * 6)
            + (sextet_c << 1 * 6)
            + (sextet_d << 0 * 6);

        if (j < *output_length) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }

    return decoded_data;
}


static int generate_YUV4MPEG(parameters_t *param)
{
  ssize_t jpegsize;
  uint32_t frame = 1;
  int loops;        /* number of loops to go */
  uint8_t* yuv[3];  /* buffer for Y/U/V planes of decoded JPEG */
  static uint8_t jpegdata[MAXPIXELS];  /* that ought to be enough */
  uint8_t first_loop = 1;
  
  read_parameters_t *read_params = malloc(sizeof(read_parameters_t));
  read_params->buff_size = 0;
  read_params->frame_buff = (uint8_t*)malloc(MAXPIXELS * sizeof(uint8_t));
  
  y4m_stream_info_t streaminfo;
  y4m_frame_info_t frameinfo;
  jpegsize = 0;
  loops = param->loop;

  char* test = "VGVzdCBpbnB1dCBlbmNvZGUgZm9yIGJhc2U2NA==";
  size_t input_len = 40;
  size_t output_len = 0;

  uint8_t* buffer = base64_decode(test, input_len, &output_len);
  mjpeg_info("Output length: %d", output_len);
  for (int i = 0; i < output_len; i++) {
      mjpeg_info("%c", buffer[i]);
  }
  base64_cleanup();


  while (1) {

    jpegsize = read_jpeg_from_stdin(jpegdata, read_params);
    if (jpegsize <= 0) {
      // sleep 50ms (or how long?)
      mjpeg_debug("No data, sleeping, buffer size: %d", read_params->buff_size);
      usleep(50 * 1000);
      continue;
    }

    if (first_loop) {
      if (init_parse_files(param, jpegdata, jpegsize)) {
        mjpeg_error_exit1("* Error processing the JPEG input.");
      }

      mjpeg_info("Now generating YUV4MPEG stream.");
      y4m_init_stream_info(&streaminfo);
      y4m_init_frame_info(&frameinfo);

      y4m_si_set_width(&streaminfo, param->width);
      y4m_si_set_height(&streaminfo, param->height);
      y4m_si_set_interlace(&streaminfo, param->interlace);
      y4m_si_set_framerate(&streaminfo, param->framerate);
      y4m_si_set_sampleaspect(&streaminfo, param->aspect_ratio);

      yuv[0] = malloc(param->width * param->height * sizeof(yuv[0][0]));
      yuv[1] = malloc(param->width * param->height / 4 * sizeof(yuv[1][0]));
      yuv[2] = malloc(param->width * param->height / 4 * sizeof(yuv[2][0]));

      y4m_write_stream_header(STDOUT_FILENO, &streaminfo);

      first_loop = 0;
    }

    /* decode_jpeg_raw:s parameters from 20010826
    * jpeg_data:       buffer with input / output jpeg
    * len:             Length of jpeg buffer
    * itype:           0: Interleaved/Progressive
    *                  1: Not-interleaved, Top field first
    *                  2: Not-interleaved, Bottom field first
    * ctype            Chroma format for decompression.
    *                  Currently always 420 and hence ignored.
    * raw0             buffer with input / output raw Y channel
    * raw1             buffer with input / output raw U/Cb channel
    * raw2             buffer with input / output raw V/Cr channel
    * width            width of Y channel (width of U/V is width/2)
    * height           height of Y channel (height of U/V is height/2)
    */

    if ((param->interlace == Y4M_ILACE_NONE) || (param->interleave == 1)) {
      mjpeg_info("Processing non-interlaced/interleaved, size %d", (int)jpegsize);
      if (param->colorspace == JCS_GRAYSCALE)
          decode_jpeg_gray_raw(jpegdata, jpegsize,
                               0, 420, param->width, param->height,
                               yuv[0], yuv[1], yuv[2]);
      else
          decode_jpeg_raw(jpegdata, jpegsize,
                          0, 420, param->width, param->height,
                          yuv[0], yuv[1], yuv[2]);
    }
    else {
      switch (param->interlace) {
      case Y4M_ILACE_TOP_FIRST:
        mjpeg_info("Processing interlaced, top-first, size %d", (int)jpegsize);
        if (param->colorspace == JCS_GRAYSCALE)
          decode_jpeg_gray_raw(jpegdata, jpegsize,
                               Y4M_ILACE_TOP_FIRST,
                               420, param->width, param->height,
                               yuv[0], yuv[1], yuv[2]);
        else
          decode_jpeg_raw(jpegdata, jpegsize,
                          Y4M_ILACE_TOP_FIRST,
                          420, param->width, param->height,
                          yuv[0], yuv[1], yuv[2]);
        break;
      case Y4M_ILACE_BOTTOM_FIRST:
        mjpeg_info("Processing interlaced, bottom-first, size %d", (int)jpegsize);
        if (param->colorspace == JCS_GRAYSCALE)
          decode_jpeg_gray_raw(jpegdata, jpegsize,
                               Y4M_ILACE_BOTTOM_FIRST,
                               420, param->width, param->height,
                               yuv[0], yuv[1], yuv[2]);
        else
          decode_jpeg_raw(jpegdata, jpegsize,
                          Y4M_ILACE_BOTTOM_FIRST,
                          420, param->width, param->height,
                          yuv[0], yuv[1], yuv[2]);
        break;
      default:
        mjpeg_error_exit1("FATAL logic error?!?");
        break;
      }
    }

    if (param->rescale_YUV) {
      mjpeg_info("Rescaling color values.");
      rescale_color_vals(param->width, param->height, yuv[0], yuv[1], yuv[2]);
    }
    mjpeg_debug("Frame decoded, now writing to output stream.");
    y4m_write_frame(STDOUT_FILENO, &streaminfo, &frameinfo, yuv);
    frame++;
  }
  
  y4m_fini_stream_info(&streaminfo);
  y4m_fini_frame_info(&frameinfo);
  free(yuv[0]);
  free(yuv[1]);
  free(yuv[2]);
  free(read_params->frame_buff);
  free(read_params);

  return 0;
}



/* main
 * in: argc, argv:  Classic commandline parameters. 
 * returns: int: 0: success, !0: !success :-)
 */
int main(int argc, char ** argv)
{ 
  parameters_t param;

  parse_commandline(argc, argv, &param);
  mjpeg_default_handler_verbosity(param.verbose);

  if (generate_YUV4MPEG(&param)) { 
    mjpeg_error_exit1("* Error processing the input files.");
  }

  return 0;
}

