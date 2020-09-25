/******************************************************************
   USB-DVG drivers for Win32 and Linux
   
   Code by Mario Montminy, 2020
   Amendments for VMMenu Chad Gray

*******************************************************************/
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#ifdef __WIN32__
   #include <windows.h>
#else
   #include <unistd.h>
   #include <sys/types.h>
   #include <sys/stat.h>
   #include <fcntl.h>
   #include <termios.h>
#endif

#include "zvgFrame.h"
//#include "timer.h"

#define ARRAY_SIZE(a)   (sizeof(a)/sizeof((a)[0]))
#define CMD_BUF_SIZE    0x20000
#define FLAG_COMPLETE   0x0
#define FLAG_RGB        0x1
#define FLAG_XY         0x2
#define FLAG_EXIT       0x7
#define FLAG_FRAME      0x4
#define FLAG_QUALITY    0x3

#define DVG_RES_MIN        0
#define DVG_RES_MAX        4095
#define DVG_RENDER_QUALITY 5

#define CONVX(x)        ((((x) - X_MIN) * DVG_RES_MAX) / (X_MAX - X_MIN))
#define CONVY(y)        ((((y) - Y_MIN) * DVG_RES_MAX) / (Y_MAX - Y_MIN))
#define MAX(x, y) (((int)(x) > (int)(y)) ? (int)(x) : (int)(y))
#define MIN(x, y) (((int)(x) < (int)(y)) ? (int)(x) : (int)(y))



#define TOP    8
#define BOTTOM 4
#define RIGHT  2
#define LEFT   1

static int     s_cmd_offs;
static uint8_t s_cmd_buf[CMD_BUF_SIZE];
static uint8_t s_last_r, s_last_g, s_last_b;
static int     s_vector_length;
static int     s_last_x;
static int     s_last_y;
#if defined(linux) || defined(__linux)
static int     s_serial_fd = -1;
static int     INVALID_HANDLE_VALUE = -1;
#else
static HANDLE  s_serial_fd = INVALID_HANDLE_VALUE;
#endif
static char    s_serial_dev[128];
static int     s_xmin, s_xmax;
static int     s_ymin, s_ymax;
extern char    DVGPort[15];

enum portErrCode
{  errOk = 0,           // no error (must be set to 0)
   errOpenCom,          // Could not open Serial Port
   errComState,         // Could not get comms state
   errSetComTimeout,    // Could not set comms timeouts
   errOpenDevice        // Could not open the DVG
};


/******************************************************************
   Calculate the length of a vector from the start and end points
*******************************************************************/
static int vector_length(int x0, int y0, int x1, int y1)
{
    int dx, dy, len;
    dx = x1 - x0;
    dy = y1 - y0;
    len = (uint32_t)sqrt((dx * dx) + (dy * dy));
    return len;
}


/******************************************************************
   Reset command
*******************************************************************/
static void cmd_reset()
{
    s_cmd_offs = 4;
    s_vector_length = 0;
    s_last_x = s_last_y = INT_MIN;
    s_last_r = s_last_g = s_last_b = 0;
}


/******************************************************************
   Open the serial port and initialise it
*******************************************************************/
static int serial_open()
{
   int result = errOpenDevice;
   #ifdef __WIN32__        // Windows code
      DCB dcb;
      COMMTIMEOUTS timeouts;
      BOOL res;
      s_serial_fd = CreateFile(s_serial_dev, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,  0, NULL);
      if (s_serial_fd == INVALID_HANDLE_VALUE)
      {
         zvgError(errOpenCom);
         goto END;
      }
      memset(&dcb, 0, sizeof(DCB));
      dcb.DCBlength = sizeof(DCB);
      res = GetCommState(s_serial_fd, &dcb);
      if (res == FALSE)
      {
         zvgError(errComState);
         goto END;
      }
      dcb.BaudRate        = 2000000;       //  Bit rate. Don't care for serial over USB.
      dcb.ByteSize        = 8;             //  Data size, xmit and rcv
      dcb.Parity          = NOPARITY;      //  Parity bit
      dcb.StopBits        = ONESTOPBIT;    //  Stop bit
      dcb.fOutX           = FALSE;
      dcb.fInX            = FALSE;
      dcb.fOutxCtsFlow    = FALSE;
      dcb.fOutxDsrFlow    = FALSE;
      dcb.fDsrSensitivity = FALSE;
      dcb.fRtsControl     = RTS_CONTROL_ENABLE;
      dcb.fDtrControl     = DTR_CONTROL_ENABLE;
      res = SetCommState(s_serial_fd, &dcb);
      if (res == FALSE)
      {
         zvgError(errSetComTimeout);
         goto END;
      }
      memset(&timeouts, 0, sizeof(COMMTIMEOUTS));
      res= SetCommTimeouts(s_serial_fd, &timeouts);
      if (res == FALSE)
      {
         zvgError(errSetComTimeout);
         goto END;
      }
      result = 0;
   #else                   // Linux Code
      struct termios attr;

      s_serial_fd = open(s_serial_dev, O_RDWR | O_NOCTTY | O_SYNC, 0666);
      if (s_serial_fd < 0)
      {
         zvgError(errOpenCom);
         goto END;
      }
      // No modem signals
      cfmakeraw(&attr);
      attr.c_cflag |= (CLOCAL | CREAD);
      attr.c_oflag &= ~OPOST;
      tcsetattr(s_serial_fd, TCSAFLUSH, &attr);
      sleep(2); //required to make flush work, for some reason
      tcflush(s_serial_fd, TCIOFLUSH);
      result = 0;
   #endif
   END:
   cmd_reset();
   return result;
}


/******************************************************************
   Write to the serial port
*******************************************************************/
static int serial_write(void *buf, uint32_t size)
{
   int result = -1;
   #ifdef __WIN32__
      DWORD written;
      if (WriteFile(s_serial_fd, buf,  size, &written, NULL))
      {
         result = written;
      }
   #else
      result = write(s_serial_fd, buf, size);
      if (result != (int)size) {
         printf("DVG: write error %d \n", result);
      }
   #endif
   return result > 0;
}


/******************************************************************
   Close the serial port
*******************************************************************/
static int serial_close()
{
   int result = -1;
   uint32_t cmd;
   if (s_serial_fd != INVALID_HANDLE_VALUE)
   {
      // Be gentle and indicate to USB-DVG that it is game over!
      cmd = (FLAG_EXIT << 29);
      s_cmd_offs = 0;
      s_cmd_buf[s_cmd_offs++] = cmd >> 24;
      s_cmd_buf[s_cmd_offs++] = cmd >> 16;
      s_cmd_buf[s_cmd_offs++] = cmd >>  8;
      s_cmd_buf[s_cmd_offs++] = cmd >>  0;
      serial_write(s_cmd_buf, s_cmd_offs);
      #ifdef __WIN32__
         CloseHandle(s_serial_fd);
      #else
         close(s_serial_fd);
      #endif
      result = 0;
   }
   s_serial_fd = INVALID_HANDLE_VALUE;
   return result;
}


/******************************************************************
   Send data to the serial port
*******************************************************************/
static int serial_send()
{
   int      result = -1;
   uint32_t cmd, chunk, size, offset;

   cmd = (FLAG_FRAME << 29) | s_vector_length;
   s_cmd_buf[0] = cmd >> 24;
   s_cmd_buf[1] = cmd >> 16;
   s_cmd_buf[2] = cmd >>  8;
   s_cmd_buf[3] = cmd >>  0;

   cmd = (FLAG_QUALITY << 29) | DVG_RENDER_QUALITY;
   s_cmd_buf[s_cmd_offs++] = cmd >> 24;
   s_cmd_buf[s_cmd_offs++] = cmd >> 16;
   s_cmd_buf[s_cmd_offs++] = cmd >>  8;
   s_cmd_buf[s_cmd_offs++] = cmd >>  0;

   cmd = (FLAG_COMPLETE << 29);
   s_cmd_buf[s_cmd_offs++] = cmd >> 24;
   s_cmd_buf[s_cmd_offs++] = cmd >> 16;
   s_cmd_buf[s_cmd_offs++] = cmd >>  8;
   s_cmd_buf[s_cmd_offs++] = cmd >>  0;
   size = s_cmd_offs;
   //printf("Buffer: %i\n",size); 
   
   offset = 0;
   while (size)
   {
      chunk   = MIN(size, 1024);
      result  = serial_write(&s_cmd_buf[offset], chunk);
      size   -= chunk;
      offset += chunk;
   }
   cmd_reset();
   return result;
}


/******************************************************************
   Function to compute region code for a point(x, y)
*******************************************************************/
uint32_t compute_code(int32_t x, int32_t y)
{
    // initialized as being inside
    uint32_t code = 0;

    if (x < s_xmin)      // to the left of rectangle
        code |= LEFT;
    else if (x > s_xmax) // to the right of rectangle
        code |= RIGHT;
    if (y < s_ymin)      // below the rectangle
        code |= BOTTOM;
    else if (y > s_ymax) // above the rectangle
        code |= TOP;

    return code;
}


/******************************************************************
   Cohen-Sutherland line-clipping algorithm.  Some games
   (such as starwars) generate coordinates outside the view window,
   so we need to clip them here.
*******************************************************************/
uint32_t line_clip(int32_t *pX1, int32_t *pY1, int32_t *pX2, int32_t *pY2)
{
   int32_t x = 0, y = 0, x1, y1, x2, y2;
   uint32_t accept, code1, code2, code_out;

   x1 = *pX1;
   y1 = *pY1;
   x2 = *pX2;
   y2 = *pY2;

   accept = 0;
   // Compute region codes for P1, P2
   code1 = compute_code(x1, y1);
   code2 = compute_code(x2, y2);

   while (1) {
      if ((code1 == 0) && (code2 == 0))
      {
         // If both endpoints lie within rectangle
         accept = 1;
         break;
      }
      else if (code1 & code2)
      {
         // If both endpoints are outside rectangle, in same region
         break;
      }
      else
      {
         // Some segment of line lies within the rectangle
         // At least one endpoint is outside the rectangle, pick it.
         if (code1 != 0)
         {
            code_out = code1;
         }
         else
         {
            code_out = code2;
         }

         // Find intersection point;
         // using formulas y = y1 + slope * (x - x1),
         // x = x1 + (1 / slope) * (y - y1)
         if (code_out & TOP)
         {
            // point is above the clip rectangle
            x = x1 + (x2 - x1) * (s_ymax - y1) / (y2 - y1);
            y = s_ymax;
         }
         else if (code_out & BOTTOM)
         {
            // point is below the rectangle
            x = x1 + (x2 - x1) * (s_ymin - y1) / (y2 - y1);
            y = s_ymin;
         }
         else if (code_out & RIGHT)
         {
            // point is to the right of rectangle
            y = y1 + (y2 - y1) * (s_xmax - x1) / (x2 - x1);
            x = s_xmax;
         }
         else if (code_out & LEFT)
         {
            // point is to the left of rectangle
            y = y1 + (y2 - y1) * (s_xmin - x1) / (x2 - x1);
            x = s_xmin;
         }

         // Now intersection point x, y is found
         // We replace point outside rectangle by intersection point
         if (code_out == code1)
         {
            x1 = x;
            y1 = y;
            code1 = compute_code(x1, y1);
         }
         else
         {
            x2 = x;
            y2 = y;
            code2 = compute_code(x2, y2);
         }
      }
   }
   *pX1 = x1;
   *pY1 = y1;
   *pX2 = x2;
   *pY2 = y2;
   return accept;
}


/******************************************************************
   Print any error messages
*******************************************************************/
void zvgError(uint32_t err)
{
   printf("DVG: ");

   switch (err)
   {
   case errOk:
      printf("No Error");
      break;
   case errOpenCom:
      printf("Error - Could not open Serial Port: %s, check hardware and port setting in vmmenu.cfg", DVGPort);
      break;
   case errComState:
      printf("Error - Could not get comms state");
      break;
   case errSetComTimeout:
      printf("Error - Could not set comms timeouts");
      break;
   case errOpenDevice:
      printf("Error - Could not open the USB-DVG");
      break;
   }
   printf("\n");
}


/******************************************************************
   Attempt to open communications to the DVG
*******************************************************************/
int zvgFrameOpen(void)
{
   int result = errOpenDevice;
   //tmrInit();                   // initialize timers
   //tmrSetFrameRate(60);         // set the frame rate
   strncpy(s_serial_dev, DVGPort, ARRAY_SIZE(s_serial_dev) - 1);
   s_serial_dev[ARRAY_SIZE(s_serial_dev) - 1] = 0;
   result = serial_open();
   return result;
}


/******************************************************************
   Close off the serial port to the device
*******************************************************************/
void zvgFrameClose( void)
{
    serial_close();
}


/******************************************************************
   Set the clip window
*******************************************************************/
void zvgFrameSetClipWin( int xMin, int yMin, int xMax, int yMax)
{
    s_xmin = xMin;
    s_ymin = yMin;
    s_xmax = xMax;
    s_ymax = yMax;
}

/******************************************************************
   Set the colour of the next vector
*******************************************************************/
void zvgFrameSetRGB15( uint8_t red, uint8_t green, uint8_t blue)
{
   uint32_t cmd;
   uint16_t r, g, b;

   // Menu uses 5 bit colour (0-31) so we multiply by 8 to make it 8 bit (0-248)
   r = red << 3;
   if (r > 255) r = 255;
   g = green << 3;
   if (g > 255) g = 255;
   b = blue << 3;
   if (b > 255) b = 255;

   s_last_r = r;
   s_last_g = g;
   s_last_b = b;

   cmd = (FLAG_RGB << 29) | ((r & 0xff) << 16) | ((g & 0xff) << 8)| (b & 0xff);
   if (s_cmd_offs <= (CMD_BUF_SIZE - 8))
   {
      s_cmd_buf[s_cmd_offs++] = cmd >> 24;
      s_cmd_buf[s_cmd_offs++] = cmd >> 16;
      s_cmd_buf[s_cmd_offs++] = cmd >>  8;
      s_cmd_buf[s_cmd_offs++] = cmd >>  0;
   }
}


/******************************************************************
   Generate the command to draw a vector 
*******************************************************************/
uint32_t zvgFrameVector( int xStart, int yStart, int xEnd, int yEnd)
{
   uint32_t cmd, blank = 0;
   int      xs, ys, xe, ye;

   if (line_clip(&xStart, &yStart, &xEnd, &yEnd))
   {
      xs = CONVX(xStart);
      ys = CONVY(yStart);
      xe = CONVX(xEnd);
      ye = CONVY(yEnd);

      if (xs > DVG_RES_MAX)
      {
         xs = DVG_RES_MAX;
      }
      if (xs < 0)
      {
         xs = 0;
      }
      if (ys > DVG_RES_MAX)
      {
         ys = DVG_RES_MAX;
      }
      if (ys < 0)
      {
         ys = 0;
      }

      s_vector_length += vector_length(s_last_x, s_last_y, xs, ys);
      s_vector_length += vector_length(xs, ys, xe, ye);

      if ((xStart != s_last_x) || (yStart != s_last_y))
      {
         blank = 1;
         cmd = (FLAG_XY << 29) | ((blank & 0x1) << 28) | ((xs & 0x3fff) << 14) | (ys & 0x3fff);
         if (s_cmd_offs <= (CMD_BUF_SIZE - 8))
         {
            s_cmd_buf[s_cmd_offs++] = cmd >> 24;
            s_cmd_buf[s_cmd_offs++] = cmd >> 16;
            s_cmd_buf[s_cmd_offs++] = cmd >>  8;
            s_cmd_buf[s_cmd_offs++] = cmd >>  0;
         }
      }

      blank = ((s_last_r == 0) && (s_last_g == 0) && (s_last_b == 0));
      cmd   = (FLAG_XY << 29) | ((blank & 0x1) << 28) | ((xe & 0x3fff) << 14) | (ye & 0x3fff);
      if (s_cmd_offs <= (CMD_BUF_SIZE - 8))
      {
         s_cmd_buf[s_cmd_offs++] = cmd >> 24;
         s_cmd_buf[s_cmd_offs++] = cmd >> 16;
         s_cmd_buf[s_cmd_offs++] = cmd >>  8;
         s_cmd_buf[s_cmd_offs++] = cmd >>  0;
      }
      s_last_x = xe;
      s_last_y = ye;
   }
   return 0;
}


/*****************************************************************************
* Send the current buffer to the DVG
*****************************************************************************/
uint32_t zvgFrameSend(void)
{
    serial_send();
    return 0;
}
