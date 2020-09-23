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
    return -1;
}


static ssize_t read_jpeg_from_stdin(uint8_t* jpegdata, read_parameters_t *read_params) {
    uint8_t buffer[MAXPIXELS];
    ssize_t read_size = 0, new_buff_size = 0, eoi_index, image_size;

    if (read_params->buff_size <= 0) {
        mjpeg_debug("Frame buffer is empty, reading from stdin...");
        read_size = fread(buffer, sizeof(unsigned char), MAXPIXELS, stdin);

        if (buffer[0] == 0xFF && buffer[1] == 0xD8) {
            mjpeg_info("SOI found!!!");
        }
        else {
            mjpeg_error_exit1("SOI not found, not JPEG file(?) or copy mistake...");
        }

        eoi_index = find_eoi_in_buffer(buffer, read_size);
        if (eoi_index == -1) {
            mjpeg_error_exit1("Failed to find EOI...");
        }
        image_size = eoi_index + 1;
        new_buff_size = read_size - image_size;
        mjpeg_debug("Input size: %d", read_size);
        mjpeg_debug("New buff size: %d", new_buff_size);

        memcpy(jpegdata, buffer, image_size * sizeof(unsigned char));

        memcpy(read_params->frame_buff, &buffer[image_size], new_buff_size * sizeof(unsigned char));
        read_params->buff_size = new_buff_size;
        return image_size;
    }
    else {
        mjpeg_debug("Searching EOI in frame buffer...");
        eoi_index = find_eoi_in_buffer(read_params->frame_buff, read_params->buff_size);

        if (eoi_index == -1) {
            mjpeg_debug("Reading again from stdin...");
            read_size = fread(buffer, sizeof(unsigned char), MAXPIXELS, stdin);

            eoi_index = find_eoi_in_buffer(buffer, read_size);
            if (eoi_index == -1) {
                mjpeg_error_exit1("Failed to find EOI...");
            }
            image_size = eoi_index + 1 + read_params->buff_size;
            memcpy(jpegdata, read_params->frame_buff, read_params->buff_size * sizeof(unsigned char));
            memcpy(jpegdata + read_params->buff_size, buffer, (eoi_index + 1) * sizeof(unsigned char));

            new_buff_size = read_size - (eoi_index + 1);
            memcpy(read_params->frame_buff, &buffer[eoi_index + 1], new_buff_size * sizeof(unsigned char));
            return image_size;
        }
        else {
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
        return -1;
    }

    read_size = fread(buffer, sizeof(unsigned char), MAXPIXELS, stdin);
    mjpeg_debug("Image size %d", read_size);

    eoi_index = find_eoi_in_buffer(buffer, read_size);
    mjpeg_info("EOI index: %d", eoi_index);
    if (buffer[eoi_index + 1] == 0xFF && buffer[eoi_index + 2] == 0xD8) {
        mjpeg_info("SOI again!!!");
    }
    memcpy(jpegdata, buffer, (eoi_index + 1) * sizeof(unsigned char));

    return eoi_index + 1;
}

static int generate_YUV4MPEG(parameters_t *param)
{
  ssize_t jpegsize;
  uint32_t frame;
  int loops;                                 /* number of loops to go */
  uint8_t* yuv[3];  /* buffer for Y/U/V planes of decoded JPEG */
  static uint8_t jpegdata[MAXPIXELS];  /* that ought to be enough */
  
  read_parameters_t *read_params = malloc(sizeof(read_parameters_t));
  read_params->buff_size = -1;
  read_params->frame_buff = (uint8_t*)malloc(MAXPIXELS * sizeof(uint8_t));
  
  y4m_stream_info_t streaminfo;
  y4m_frame_info_t frameinfo;
  jpegsize = 0;
  loops = param->loop;

  mjpeg_info("Reading first image");
  jpegsize = read_jpeg_from_stdin(jpegdata, read_params);
  if (jpegsize > 0) {
      mjpeg_debug("Managed to read image from stdin...");
  }
  else {
      mjpeg_error_exit1("Error reading from stdin...");
  }

  if (init_parse_files(param, jpegdata, jpegsize)) {
      mjpeg_error_exit1("* Error processing the JPEG input.");
  }

  mjpeg_info("Number of Loops %i", loops);
  mjpeg_info("Number of Frames %i", param->numframes);
  mjpeg_info("Start at frame %i", param->begin);

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
 

  param->numframes = 5;
  do {
     for (frame = param->begin;
          (frame < param->numframes + param->begin) || (param->numframes == -1);
          frame++) {
       
       if (frame > param->begin) {
         jpegsize = read_jpeg_from_stdin(jpegdata, read_params);
       }

       mjpeg_debug("Numframes %i  jpegsize %d", param->numframes, (int)jpegsize);
       if (jpegsize <= 0) {
         mjpeg_info("jpeg size %d...", jpegsize);
         mjpeg_debug("in jpegsize <= 0"); 
         if (param->numframes == -1)
            {
            mjpeg_info("No more frames.  Stopping.");
            break;  /* we are done; leave 'while' loop */
            }
         else
            mjpeg_info("Rewriting latest frame instead.");
       }
         
       if (jpegsize > 0) {
         mjpeg_debug("Preparing frame");
         
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
         } else {
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

	 if (param->rescale_YUV)
	   {
	     mjpeg_info("Rescaling color values.");
	     rescale_color_vals(param->width, param->height, yuv[0], yuv[1], yuv[2]);
	   }
	 mjpeg_debug("Frame decoded, now writing to output stream.");
       }
   
       y4m_write_frame(STDOUT_FILENO, &streaminfo, &frameinfo, yuv);
     }
     if (param->loop != -1)
       loops--;
 
  } while( loops >=1 || loops == -1 );
  
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

