/*
 * Copyright © 2006 Novell, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of
 * Novell, Inc. not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior permission.
 * Novell, Inc. makes no representations about the suitability of this
 * software for any purpose. It is provided "as is" without express or
 * implied warranty.
 *
 * NOVELL, INC. DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL NOVELL, INC. BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Author: David Reveman <davidr@novell.com>
 *         Michail Bitzes <noodlylight@gmail.com>
 */

#ifdef HAVE_CONFIG_H
#  include "../config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <fusilli-core.h>

#define TEXTURE_SIZE 256

#define K 0.1964f

#define TEXTURE_NUM 3

typedef struct _WaterFunction {
	struct _WaterFunction *next;

	int handle;
	int target;
	int param;
	int unit;
} WaterFunction;

#define TINDEX(ws, i) (((ws)->tIndex + (i)) % TEXTURE_NUM)

#define CLAMP(v, min, max) \
	if ((v) > (max))       \
		(v) = (max);       \
	else if ((v) < (min))  \
		(v) = (min)

#define WATER_INITIATE_MODIFIERS_DEFAULT (ControlMask | CompSuperMask)

static int bananaIndex;

static int initiate_key_modifiers;

static int displayPrivateIndex;

static CompKeyBinding toggle_rain_key, toggle_wiper_key;

static int waterLastPointerX = 0;
static int waterLastPointerY = 0;

typedef struct _WaterDisplay {
	int             screenPrivateIndex;

	HandleEventProc handleEvent;

	float           offsetScale;
} WaterDisplay;

typedef struct _WaterScreen {
	PreparePaintScreenProc preparePaintScreen;
	DonePaintScreenProc    donePaintScreen;
	DrawWindowTextureProc  drawWindowTexture;

	int grabIndex;
	int width, height;

	GLuint program;
	GLuint texture[TEXTURE_NUM];

	int     tIndex;
	GLenum  target;
	GLfloat tx, ty;

	int count;

	GLuint fbo;
	GLint  fboStatus;

	void          *data;
	float         *d0;
	float         *d1;
	unsigned char *t0;

	CompTimeoutHandle rainHandle;
	CompTimeoutHandle wiperHandle;

	float wiperAngle;
	float wiperSpeed;

	WaterFunction *bumpMapFunctions;
} WaterScreen;

#define GET_WATER_DISPLAY(d) \
        ((WaterDisplay *) (d)->privates[displayPrivateIndex].ptr)

#define WATER_DISPLAY(d) \
        WaterDisplay *wd = GET_WATER_DISPLAY (d)

#define GET_WATER_SCREEN(s, wd) \
        ((WaterScreen *) (s)->privates[(wd)->screenPrivateIndex].ptr)

#define WATER_SCREEN(s) \
        WaterScreen *ws = GET_WATER_SCREEN (s, GET_WATER_DISPLAY (&display))

static Bool
waterRainTimeout (void *closure);

static Bool
waterWiperTimeout (void *closure);

static const char *waterFpString =
	"!!ARBfp1.0"

	"PARAM param = program.local[0];"
	"ATTRIB t11  = fragment.texcoord[0];"

	"TEMP t01, t21, t10, t12;"
	"TEMP c11, c01, c21, c10, c12;"
	"TEMP prev, v, temp, accel;"

	"TEX prev, t11, texture[0], %s;"
	"TEX c11,  t11, texture[1], %s;"

	/* sample offsets */
	"ADD t01, t11, { - %f, 0.0, 0.0, 0.0 };"
	"ADD t21, t11, {   %f, 0.0, 0.0, 0.0 };"
	"ADD t10, t11, { 0.0, - %f, 0.0, 0.0 };"
	"ADD t12, t11, { 0.0,   %f, 0.0, 0.0 };"

	/* fetch nesseccary samples */
	"TEX c01, t01, texture[1], %s;"
	"TEX c21, t21, texture[1], %s;"
	"TEX c10, t10, texture[1], %s;"
	"TEX c12, t12, texture[1], %s;"

	/* x/y normals from height */
	"MOV v, { 0.0, 0.0, 0.75, 0.0 };"
	"SUB v.x, c12.w, c10.w;"
	"SUB v.y, c01.w, c21.w;"

	/* bumpiness */
	"MUL v, v, 1.5;"

	/* normalize */
	"MAD temp, v.x, v.x, 1.0;"
	"MAD temp, v.y, v.y, temp;"
	"RSQ temp, temp.x;"
	"MUL v, v, temp;"

	/* add scale and bias to normal */
	"MAD v, v, 0.5, 0.5;"

	/* done with computing the normal, continue with computing the next
	   height value */
	"ADD accel, c10, c12;"
	"ADD accel, c01, accel;"
	"ADD accel, c21, accel;"
	"MAD accel, -4.0, c11, accel;"

	/* store new height in alpha component */
	"MAD v.w, 2.0, c11, -prev.w;"
	"MAD v.w, accel, param.x, v.w;"

	/* fade out height */
	"MUL v.w, v.w, param.y;"

	"MOV result.color, v;"

	"END";

static int
loadFragmentProgram (CompScreen *s,
                     GLuint     *program,
                     const char *string)
{
	GLint errorPos;

	/* clear errors */
	glGetError ();

	if (!*program)
		(*s->genPrograms) (1, program);

	(*s->bindProgram) (GL_FRAGMENT_PROGRAM_ARB, *program);
	(*s->programString) (GL_FRAGMENT_PROGRAM_ARB,
	                     GL_PROGRAM_FORMAT_ASCII_ARB,
	                     strlen (string), string);

	glGetIntegerv (GL_PROGRAM_ERROR_POSITION_ARB, &errorPos);
	if (glGetError () != GL_NO_ERROR || errorPos != -1)
	{
		compLogMessage ("water", CompLogLevelError,
		                "failed to load bump map program");

		(*s->deletePrograms) (1, program);
		*program = 0;

		return 0;
	}

	return 1;
}

static int
loadWaterProgram (CompScreen *s)
{
	char buffer[1024];

	WATER_SCREEN (s);

	if (ws->target == GL_TEXTURE_2D)
		sprintf (buffer, waterFpString,
		         "2D", "2D",
		          1.0f / ws->width,  1.0f / ws->width,
		          1.0f / ws->height, 1.0f / ws->height,
		         "2D", "2D", "2D", "2D");
	else
		sprintf (buffer, waterFpString,
		         "RECT", "RECT",
		         1.0f, 1.0f, 1.0f, 1.0f,
		         "RECT", "RECT", "RECT", "RECT");

	return loadFragmentProgram (s, &ws->program, buffer);
}

static int
getBumpMapFragmentFunction (CompScreen  *s,
                            CompTexture *texture,
                            int         unit,
                            int         param)
{
	WaterFunction    *function;
	CompFunctionData *data;
	int              target;

	WATER_SCREEN (s);

	if (texture->target == GL_TEXTURE_2D)
		target = COMP_FETCH_TARGET_2D;
	else
		target = COMP_FETCH_TARGET_RECT;

	for (function = ws->bumpMapFunctions; function; function = function->next)
	{
		if (function->param  == param &&
		    function->unit   == unit  &&
		    function->target == target)
			return function->handle;
	}

	data = createFunctionData ();
	if (data)
	{
		static char *temp[] = { "normal", "temp", "total", "bump", "offset" };
		int         i, handle = 0;
		char        str[1024];

		for (i = 0; i < sizeof (temp) / sizeof (temp[0]); i++)
		{
			if (!addTempHeaderOpToFunctionData (data, temp[i]))
			{
				destroyFunctionData (data);
				return 0;
			}
		}

		snprintf (str, 1024,

		          /* get normal from normal map */
		          "TEX normal, fragment.texcoord[%d], texture[%d], %s;"

		          /* save height */
		          "MOV offset, normal;"

		          /* remove scale and bias from normal */
		          "MAD normal, normal, 2.0, -1.0;"

		          /* normalize the normal map */
		          "DP3 temp, normal, normal;"
		          "RSQ temp, temp.x;"
		          "MUL normal, normal, temp;"

		          /* scale down normal by height and constant and use as
		             offset in texture */
		          "MUL offset, normal, offset.w;"
		          "MUL offset, offset, program.env[%d];",

		          unit, unit,
		          (ws->target == GL_TEXTURE_2D) ? "2D" : "RECT",
		          param);

		if (!addDataOpToFunctionData (data, str))
		{
			destroyFunctionData (data);
			return 0;
		}

		if (!addFetchOpToFunctionData (data, "output", "offset.yxzz", target))
		{
			destroyFunctionData (data);
			return 0;
		}

		snprintf (str, 1024,

		          /* normal dot lightdir, this should eventually be
		        	 changed to a real light vector */
		          "DP3 bump, normal, { 0.707, 0.707, 0.0, 0.0 };"
		          "MUL bump, bump, state.light[0].diffuse;");

		if (!addDataOpToFunctionData (data, str))
		{
			destroyFunctionData (data);
			return 0;
		}

		if (!addColorOpToFunctionData (data, "output", "output"))
		{
			destroyFunctionData (data);
			return 0;
		}

		snprintf (str, 1024,

		          /* diffuse per-vertex lighting, opacity and brightness
		        	 and add lightsource bump color */
		          "ADD output, output, bump;");

		if (!addDataOpToFunctionData (data, str))
		{
			destroyFunctionData (data);
			return 0;
		}

		function = malloc (sizeof (WaterFunction));
		if (function)
		{
			handle = createFragmentFunction (s, "water", data);

			function->handle = handle;
			function->target = target;
			function->param  = param;
			function->unit   = unit;

			function->next = ws->bumpMapFunctions;
			ws->bumpMapFunctions = function;
		}

		destroyFunctionData (data);

		return handle;
	}

	return 0;
}

static void
allocTexture (CompScreen *s,
              int        index)
{
	WATER_SCREEN (s);

	glGenTextures (1, &ws->texture[index]);
	glBindTexture (ws->target, ws->texture[index]);

	glTexParameteri (ws->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (ws->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (ws->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (ws->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glTexImage2D (ws->target,
	              0,
	              GL_RGBA,
	              ws->width,
	              ws->height,
	              0,
	              GL_BGRA,

#if IMAGE_BYTE_ORDER == MSBFirst
	              GL_UNSIGNED_INT_8_8_8_8_REV,
#else
	              GL_UNSIGNED_BYTE,
#endif

	              ws->t0);

	glBindTexture (ws->target, 0);
}

static int
fboPrologue (CompScreen *s,
             int        tIndex)
{
	WATER_SCREEN (s);

	if (!ws->fbo)
		return 0;

	if (!ws->texture[tIndex])
		allocTexture (s, tIndex);

	(*s->bindFramebuffer) (GL_FRAMEBUFFER_EXT, ws->fbo);

	(*s->framebufferTexture2D) (GL_FRAMEBUFFER_EXT,
	                        GL_COLOR_ATTACHMENT0_EXT,
	                        ws->target, ws->texture[tIndex],
	                        0);

	glDrawBuffer (GL_COLOR_ATTACHMENT0_EXT);
	glReadBuffer (GL_COLOR_ATTACHMENT0_EXT);

	/* check status the first time */
	if (!ws->fboStatus)
	{
		ws->fboStatus = (*s->checkFramebufferStatus) (GL_FRAMEBUFFER_EXT);
		if (ws->fboStatus != GL_FRAMEBUFFER_COMPLETE_EXT)
		{
			compLogMessage ("water", CompLogLevelError,
			                "framebuffer incomplete");

			(*s->bindFramebuffer) (GL_FRAMEBUFFER_EXT, 0);
			(*s->deleteFramebuffers) (1, &ws->fbo);

			glDrawBuffer (GL_BACK);
			glReadBuffer (GL_BACK);

			ws->fbo = 0;

			return 0;
		}
	}

	glViewport (0, 0, ws->width, ws->height);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();

	return 1;
}

static void
fboEpilogue (CompScreen *s)
{
	(*s->bindFramebuffer) (GL_FRAMEBUFFER_EXT, 0);

	glMatrixMode (GL_PROJECTION);
	glLoadIdentity ();
	glMatrixMode (GL_MODELVIEW);
	glLoadIdentity ();
	glDepthRange (0, 1);
	glViewport (-1, -1, 2, 2);
	glRasterPos2f (0, 0);

	s->rasterX = s->rasterY = 0;

	setDefaultViewport (s);

	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();

	glDrawBuffer (GL_BACK);
	glReadBuffer (GL_BACK);
}

static int
fboUpdate (CompScreen *s,
           float      dt,
           float      fade)
{
	WATER_SCREEN (s);

	if (!fboPrologue (s, TINDEX (ws, 1)))
		return 0;

	if (!ws->texture[TINDEX (ws, 2)])
		allocTexture (s, TINDEX (ws, 2));

	if (!ws->texture[TINDEX (ws, 0)])
		allocTexture (s, TINDEX (ws, 0));

	glEnable (ws->target);

	(*s->activeTexture) (GL_TEXTURE0_ARB);
	glBindTexture (ws->target, ws->texture[TINDEX (ws, 2)]);

	glTexParameteri (ws->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (ws->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	(*s->activeTexture) (GL_TEXTURE1_ARB);
	glBindTexture (ws->target, ws->texture[TINDEX (ws, 0)]);
	glTexParameteri (ws->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (ws->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glEnable (GL_FRAGMENT_PROGRAM_ARB);
	(*s->bindProgram) (GL_FRAGMENT_PROGRAM_ARB, ws->program);

	(*s->programLocalParameter4f) (GL_FRAGMENT_PROGRAM_ARB, 0,
	                           dt * K, fade, 1.0f, 1.0f);

	glBegin (GL_QUADS);

	glTexCoord2f (0.0f, 0.0f);
	glVertex2f   (0.0f, 0.0f);
	glTexCoord2f (ws->tx, 0.0f);
	glVertex2f   (1.0f, 0.0f);
	glTexCoord2f (ws->tx, ws->ty);
	glVertex2f   (1.0f, 1.0f);
	glTexCoord2f (0.0f, ws->ty);
	glVertex2f   (0.0f, 1.0f);

	glEnd ();

	glDisable (GL_FRAGMENT_PROGRAM_ARB);

	glTexParameteri (ws->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (ws->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture (ws->target, 0);
	(*s->activeTexture) (GL_TEXTURE0_ARB);
	glTexParameteri (ws->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (ws->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture (ws->target, 0);

	glDisable (ws->target);

	fboEpilogue (s);

	/* increment texture index */
	ws->tIndex = TINDEX (ws, 1);

	return 1;
}

static int
fboVertices (CompScreen *s,
             GLenum     type,
             XPoint     *p,
             int        n,
             float      v)
{
	WATER_SCREEN (s);

	if (!fboPrologue (s, TINDEX (ws, 0)))
		return 0;

	glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glColor4f (0.0f, 0.0f, 0.0f, v);

	glPointSize (3.0f);
	glLineWidth (1.0f);

	glScalef (1.0f / ws->width, 1.0f / ws->height, 1.0);
	glTranslatef (0.5f, 0.5f, 0.0f);

	glBegin (type);

	while (n--)
	{
		glVertex2i (p->x, p->y);
		p++;
	}

	glEnd ();

	glColor4usv (defaultColor);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	fboEpilogue (s);

	return 1;
}

static void
softwareUpdate (CompScreen *s,
                float      dt,
                float      fade)
{
	float      *dTmp;
	int        i, j;
	float      v0, v1, inv;
	float      accel, value;
	unsigned char *t0, *t;
	int        dWidth, dHeight;
	float      *d01, *d10, *d11, *d12;

	WATER_SCREEN (s);

	if (!ws->texture[TINDEX (ws, 0)])
		allocTexture (s, TINDEX (ws, 0));

	dt *= K * 2.0f;
	fade *= 0.99f;

	dWidth  = ws->width  + 2;
	dHeight = ws->height + 2;

#define D(d, j) (*((d) + (j)))

	d01 = ws->d0 + dWidth;
	d10 = ws->d1;
	d11 = d10 + dWidth;
	d12 = d11 + dWidth;

	for (i = 1; i < dHeight - 1; i++)
	{
		for (j = 1; j < dWidth - 1; j++)
		{
			accel = dt * (D (d10, j)     +
			              D (d12, j)     +
			              D (d11, j - 1) +
			              D (d11, j + 1) - 4.0f * D (d11, j));

			value = (2.0f * D (d11, j) - D (d01, j) + accel) * fade;

			CLAMP (value, 0.0f, 1.0f);

			D (d01, j) = value;
		}

		d01 += dWidth;
		d10 += dWidth;
		d11 += dWidth;
		d12 += dWidth;
	}

	/* update border */
	memcpy (ws->d0, ws->d0 + dWidth, dWidth * sizeof (GLfloat));
	memcpy (ws->d0 + dWidth * (dHeight - 1),
	        ws->d0 + dWidth * (dHeight - 2),
	        dWidth * sizeof (GLfloat));

	d01 = ws->d0 + dWidth;

	for (i = 1; i < dHeight - 1; i++)
	{
		D (d01, 0)          = D (d01, 1);
		D (d01, dWidth - 1) = D (d01, dWidth - 2);

		d01 += dWidth;
	}

	d10 = ws->d1;
	d11 = d10 + dWidth;
	d12 = d11 + dWidth;

	t0 = ws->t0;

	/* update texture */
	for (i = 0; i < ws->height; i++)
	{
		for (j = 0; j < ws->width; j++)
		{
			v0 = (D (d12, j)     - D (d10, j))     * 1.5f;
			v1 = (D (d11, j - 1) - D (d11, j + 1)) * 1.5f;

			/* 0.5 for scale */
			inv = 0.5f / sqrtf (v0 * v0 + v1 * v1 + 1.0f);

			/* add scale and bias to normal */
			v0 = v0 * inv + 0.5f;
			v1 = v1 * inv + 0.5f;

			/* store normal map in RGB components */
			t = t0 + (j * 4);
			t[0] = (unsigned char) ((inv + 0.5f) * 255.0f);
			t[1] = (unsigned char) (v1 * 255.0f);
			t[2] = (unsigned char) (v0 * 255.0f);

			/* store height in A component */
			t[3] = (unsigned char) (D (d11, j) * 255.0f);
		}

		d10 += dWidth;
		d11 += dWidth;
		d12 += dWidth;

		t0 += ws->width * 4;
	}

#undef D

	/* swap height maps */
	dTmp   = ws->d0;
	ws->d0 = ws->d1;
	ws->d1 = dTmp;

	if (ws->texture[TINDEX (ws, 0)])
	{
		glBindTexture (ws->target, ws->texture[TINDEX (ws, 0)]);
		glTexImage2D (ws->target,
		              0,
		              GL_RGBA,
		              ws->width,
		              ws->height,
		              0,
		              GL_BGRA,

#if IMAGE_BYTE_ORDER == MSBFirst
		              GL_UNSIGNED_INT_8_8_8_8_REV,
#else
		              GL_UNSIGNED_BYTE,
#endif

		              ws->t0);
	}
}


#define SET(x, y, v) *((ws->d1) + (ws->width + 2) * (y + 1) + (x + 1)) = (v)

static void
softwarePoints (CompScreen *s,
                XPoint     *p,
                int        n,
                float      add)
{
	WATER_SCREEN (s);

	while (n--)
	{
		SET (p->x - 1, p->y - 1, add);
		SET (p->x, p->y - 1, add);
		SET (p->x + 1, p->y - 1, add);

		SET (p->x - 1, p->y, add);
		SET (p->x, p->y, add);
		SET (p->x + 1, p->y, add);

		SET (p->x - 1, p->y + 1, add);
		SET (p->x, p->y + 1, add);
		SET (p->x + 1, p->y + 1, add);

		p++;
	}
}

/* bresenham */
static void
softwareLines (CompScreen *s,
               XPoint     *p,
               int        n,
               float      v)
{
	int	 x1, y1, x2, y2;
	Bool steep;
	int  tmp;
	int  deltaX, deltaY;
	int  error = 0;
	int  yStep;
	int  x, y;

	WATER_SCREEN (s);

#define SWAP(v0, v1) \
	tmp = v0;        \
	v0 = v1;         \
	v1 = tmp

	while (n > 1)
	{
		x1 = p->x;
		y1 = p->y;

		p++;
		n--;

		x2 = p->x;
		y2 = p->y;

		p++;
		n--;

		steep = abs (y2 - y1) > abs (x2 - x1);
		if (steep)
		{
			SWAP (x1, y1);
			SWAP (x2, y2);
		}

		if (x1 > x2)
		{
			SWAP (x1, x2);
			SWAP (y1, y2);
		}

#undef SWAP

		deltaX = x2 - x1;
		deltaY = abs (y2 - y1);

		y = y1;
		if (y1 < y2)
			yStep = 1;
		else
			yStep = -1;

		for (x = x1; x <= x2; x++)
		{
			if (steep)
			{
				SET (y, x, v);
			}
			else
			{
				SET (x, y, v);
			}

			error += deltaY;
			if (2 * error >= deltaX)
			{
				y += yStep;
				error -= deltaX;
			}
		}
	}
}

#undef SET

static void
softwareVertices (CompScreen *s,
                  GLenum     type,
                  XPoint     *p,
                  int        n,
                  float      v)
{
	switch (type) {
	case GL_POINTS:
		softwarePoints (s, p, n, v);
		break;
	case GL_LINES:
		softwareLines (s, p, n, v);
		break;
	}
}

static void
waterUpdate (CompScreen *s,
             float      dt)
{
	GLfloat fade = 1.0f;

	WATER_SCREEN (s);

	if (ws->count < 1000)
	{
		if (ws->count > 1)
			fade = 0.90f + ws->count / 10000.0f;
		else
			fade = 0.0f;
	}

	if (!fboUpdate (s, dt, fade))
		softwareUpdate (s, dt, fade);
}

static void
scaleVertices (CompScreen *s,
               XPoint     *p,
               int        n)
{
	WATER_SCREEN (s);

	while (n--)
	{
		p[n].x = (ws->width  * p[n].x) / s->width;
		p[n].y = (ws->height * p[n].y) / s->height;
	}
}

static void
waterVertices (CompScreen *s,
               GLenum     type,
               XPoint     *p,
               int        n,
               float      v)
{
	WATER_SCREEN (s);

	if (!s->fragmentProgram)
		return;

	scaleVertices (s, p, n);

	if (!fboVertices (s, type, p, n, v))
		softwareVertices (s, type, p, n, v);

	if (ws->count < 3000)
		ws->count = 3000;
}

static Bool
waterRainTimeout (void *closure)
{
	CompScreen *s = closure;
	XPoint     p;

	p.x = (int) (s->width  * (rand () / (float) RAND_MAX));
	p.y = (int) (s->height * (rand () / (float) RAND_MAX));

	waterVertices (s, GL_POINTS, &p, 1, 0.8f * (rand () / (float) RAND_MAX));

	damageScreen (s);

	return TRUE;
}

static Bool
waterWiperTimeout (void *closure)
{
	CompScreen *s = closure;

	WATER_SCREEN (s);

	if (ws->count)
	{
		if (ws->wiperAngle == 0.0f)
			ws->wiperSpeed = 2.5f;
		else if (ws->wiperAngle == 180.0f)
			ws->wiperSpeed = -2.5f;
	}

	return TRUE;
}

static void
waterReset (CompScreen *s)
{
	int size, i, j;

	WATER_SCREEN (s);

	ws->height = TEXTURE_SIZE;
	ws->width  = (ws->height * s->width) / s->height;

	if (s->textureNonPowerOfTwo ||
	    (POWER_OF_TWO (ws->width) && POWER_OF_TWO (ws->height)))
	{
		ws->target = GL_TEXTURE_2D;
		ws->tx = ws->ty = 1.0f;
	}
	else
	{
		ws->target = GL_TEXTURE_RECTANGLE_NV;
		ws->tx = ws->width;
		ws->ty = ws->height;
	}

	if (!s->fragmentProgram)
		return;

	if (s->fbo)
	{
		loadWaterProgram (s);
		if (!ws->fbo)
			(*s->genFramebuffers) (1, &ws->fbo);
	}

	ws->fboStatus = 0;

	for (i = 0; i < TEXTURE_NUM; i++)
	{
		if (ws->texture[i])
		{
			glDeleteTextures (1, &ws->texture[i]);
			ws->texture[i] = 0;
		}
	}

	if (ws->data)
		free (ws->data);

	size = (ws->width + 2) * (ws->height + 2);

	ws->data = calloc (1, (sizeof (float) * size * 2) +
	                   (sizeof (GLubyte) * ws->width * ws->height * 4));
	if (!ws->data)
		return;

	ws->d0 = ws->data;
	ws->d1 = (ws->d0 + (size));
	ws->t0 = (unsigned char *) (ws->d1 + (size));

	for (i = 0; i < ws->height; i++)
	{
		for (j = 0; j < ws->width; j++)
		{
			(ws->t0 + (ws->width * 4 * i + j * 4))[0] = 0xff;
		}
	}
}

static void
waterDrawWindowTexture (CompWindow           *w,
                        CompTexture          *texture,
                        const FragmentAttrib *attrib,
                        unsigned int         mask)
{
	WATER_SCREEN (w->screen);

	if (ws->count)
	{
		FragmentAttrib fa = *attrib;
		Bool           lighting = w->screen->lighting;
		int            param, function, unit;
		GLfloat        plane[4];

		WATER_DISPLAY (&display);

		param = allocFragmentParameters (&fa, 1);
		unit  = allocFragmentTextureUnits (&fa, 1);

		function = getBumpMapFragmentFunction (w->screen, texture, unit, param);
		if (function)
		{
			addFragmentFunction (&fa, function);

			screenLighting (w->screen, TRUE);

			(*w->screen->activeTexture) (GL_TEXTURE0_ARB + unit);

			glBindTexture (ws->target, ws->texture[TINDEX (ws, 0)]);

			plane[1] = plane[2] = 0.0f;
			plane[0] = ws->tx / (GLfloat) w->screen->width;
			plane[3] = 0.0f;

			glTexGeni (GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
			glTexGenfv (GL_S, GL_EYE_PLANE, plane);
			glEnable (GL_TEXTURE_GEN_S);

			plane[0] = plane[2] = 0.0f;
			plane[1] = ws->ty / (GLfloat) w->screen->height;
			plane[3] = 0.0f;

			glTexGeni (GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
			glTexGenfv (GL_T, GL_EYE_PLANE, plane);
			glEnable (GL_TEXTURE_GEN_T);

			(*w->screen->activeTexture) (GL_TEXTURE0_ARB);

			(*w->screen->programEnvParameter4f) (GL_FRAGMENT_PROGRAM_ARB, param,
			                         texture->matrix.yy *
			                         wd->offsetScale,
			                         -texture->matrix.xx *
			                         wd->offsetScale,
			                         0.0f, 0.0f);
		}

		/* to get appropriate filtering of texture */
		mask |= PAINT_WINDOW_ON_TRANSFORMED_SCREEN_MASK;

		UNWRAP (ws, w->screen, drawWindowTexture);
		(*w->screen->drawWindowTexture) (w, texture, &fa, mask);
		WRAP (ws, w->screen, drawWindowTexture, waterDrawWindowTexture);

		if (function)
		{
			(*w->screen->activeTexture) (GL_TEXTURE0_ARB + unit);
			glDisable (GL_TEXTURE_GEN_T);
			glDisable (GL_TEXTURE_GEN_S);
			glBindTexture (ws->target, 0);
			(*w->screen->activeTexture) (GL_TEXTURE0_ARB);

			screenLighting (w->screen, lighting);
		}
	}
	else
	{
		UNWRAP (ws, w->screen, drawWindowTexture);
		(*w->screen->drawWindowTexture) (w, texture, attrib, mask);
		WRAP (ws, w->screen, drawWindowTexture, waterDrawWindowTexture);
	}
}

/* TODO: a way to control the speed */
static void
waterPreparePaintScreen (CompScreen *s,
                         int        msSinceLastPaint)
{
	WATER_SCREEN (s);

	if (ws->count)
	{
		ws->count -= 10;
		if (ws->count < 0)
			ws->count = 0;

		if (ws->wiperHandle)
		{
			float  step, angle0, angle1;
			Bool   wipe = FALSE;
			XPoint p[3];

			p[1].x = s->width / 2;
			p[1].y = s->height;

			step = ws->wiperSpeed * msSinceLastPaint / 20.0f;

			if (ws->wiperSpeed > 0.0f)
			{
				if (ws->wiperAngle < 180.0f)
				{
					angle0 = ws->wiperAngle;

					ws->wiperAngle += step;
					ws->wiperAngle = MIN (ws->wiperAngle, 180.0f);

					angle1 = ws->wiperAngle;

					wipe = TRUE;
				}
			}
			else
			{
				if (ws->wiperAngle > 0.0f)
				{
					angle1 = ws->wiperAngle;

					ws->wiperAngle += step;
					ws->wiperAngle = MAX (ws->wiperAngle, 0.0f);

					angle0 = ws->wiperAngle;

					wipe = TRUE;
				}
			}

#define TAN(a) (tanf ((a) * (M_PI / 180.0f)))

			if (wipe)
			{
				if (angle0 > 0.0f)
				{
					p[2].x = s->width / 2 - s->height / TAN (angle0);
					p[2].y = 0;
				}
				else
				{
					p[2].x = 0;
					p[2].y = s->height;
				}

				if (angle1 < 180.0f)
				{
					p[0].x = s->width / 2 - s->height / TAN (angle1);
					p[0].y = 0;
				}
				else
				{
					p[0].x = s->width;
					p[0].y = s->height;
				}

				/* software rasterizer doesn't support triangles yet so wiper
				   effect will only work with FBOs right now */
				waterVertices (s, GL_TRIANGLES, p, 3, 0.0f);
			}

#undef TAN

		}

		waterUpdate (s, 0.8f);
	}

	UNWRAP (ws, s, preparePaintScreen);
	(*s->preparePaintScreen) (s, msSinceLastPaint);
	WRAP (ws, s, preparePaintScreen, waterPreparePaintScreen);
}

static void
waterDonePaintScreen (CompScreen *s)
{
	WATER_SCREEN (s);

	if (ws->count)
		damageScreen (s);

	UNWRAP (ws, s, donePaintScreen);
	(*s->donePaintScreen) (s);
	WRAP (ws, s, donePaintScreen, waterDonePaintScreen);
}

static void
waterHandleMotionEvent (CompDisplay *d,
                        Window      root)
{
	CompScreen *s;

	s = findScreenAtDisplay (root);
	if (s)
	{
		WATER_SCREEN (s);

		if (ws->grabIndex)
		{
			XPoint p[2];

			p[0].x = waterLastPointerX;
			p[0].y = waterLastPointerY;

			p[1].x = waterLastPointerX = pointerX;
			p[1].y = waterLastPointerY = pointerY;

			waterVertices (s, GL_LINES, p, 2, 0.2f);

			damageScreen (s);
		}
	}
}

static Bool
waterInitiate (BananaArgument     *arg,
               int                nArg)
{
	CompScreen   *s;
	unsigned int ui;
	Window       root, child;
	int          xRoot, yRoot, i;

	for (s = display.screens; s; s = s->next)
	{
		WATER_SCREEN (s);

		if (otherScreenGrabExist (s, "water", NULL))
			continue;

		if (!ws->grabIndex)
			ws->grabIndex = pushScreenGrab (s, None, "water");

		if (XQueryPointer (display.display, s->root, &root, &child, 
		           &xRoot, &yRoot, &i, &i, &ui))
		{
			XPoint p;

			p.x = waterLastPointerX = xRoot;
			p.y = waterLastPointerY = yRoot;

			waterVertices (s, GL_POINTS, &p, 1, 0.8f);

			damageScreen (s);
		}
	}

	return FALSE;
}

static Bool
waterTerminate (BananaArgument     *arg,
                int                nArg)
{
	CompScreen *s;

	for (s = display.screens; s; s = s->next)
	{
		WATER_SCREEN (s);

		if (ws->grabIndex)
		{
			removeScreenGrab (s, ws->grabIndex, 0);
			ws->grabIndex = 0;
		}
	}

	return FALSE;
}

static Bool
waterToggleRain (BananaArgument     *arg,
                 int                nArg)
{
	CompScreen *s;
	Window     xid;

	BananaValue *root = getArgNamed ("root", arg, nArg);

	if (root != NULL)
		xid = root->i;
	else
		xid = 0;

	s = findScreenAtDisplay (xid);
	if (s)
	{
		WATER_SCREEN (s);

		if (!ws->rainHandle)
		{
			int delay;

			const BananaValue *
			option_rain_delay = bananaGetOption (bananaIndex,
			                                     "rain_delay",
			                                     -1);

			delay = option_rain_delay->i;
			ws->rainHandle = compAddTimeout (delay, (float) delay * 1.2,
			                         waterRainTimeout, s);
		}
		else
		{
			compRemoveTimeout (ws->rainHandle);
			ws->rainHandle = 0;
		}
	}

	return FALSE;
}

static Bool
waterToggleWiper (BananaArgument     *arg,
                  int                nArg)
{
	CompScreen *s;
	Window     xid;

	BananaValue *root = getArgNamed ("root", arg, nArg);

	if (root != NULL)
		xid = root->i;
	else
		xid = 0;

	s = findScreenAtDisplay (xid);
	if (s)
	{
		WATER_SCREEN (s);

		if (!ws->wiperHandle)
		{
			ws->wiperHandle = compAddTimeout (2000, 2400, waterWiperTimeout, s);
		}
		else
		{
			compRemoveTimeout (ws->wiperHandle);
			ws->wiperHandle = 0;
		}
	}

	return FALSE;
}

static Bool
waterTitleWave (BananaArgument     *arg,
                int                nArg)
{
	CompWindow *w;
	int        xid;

	BananaValue *window = getArgNamed ("window", arg, nArg);

	if (window != NULL)
		xid = window->i;
	else
		xid = display.activeWindow;

	w = findWindowAtDisplay (xid);
	if (w)
	{
		XPoint p[2];

		p[0].x = w->attrib.x - w->input.left;
		p[0].y = w->attrib.y - w->input.top / 2;

		p[1].x = w->attrib.x + w->width + w->input.right;
		p[1].y = p[0].y;

		waterVertices (w->screen, GL_LINES, p, 2, 0.15f);

		damageScreen (w->screen);
	}

	return FALSE;
}
/* TODO: needs fixing
static Bool
waterPoint (BananaArgument     *arg,
            int                nArg)
{
	CompScreen *s;
	int        xid;

	xid = getIntOptionNamed (option, nOption, "root", 0);

	s = findScreenAtDisplay (d, xid);
	if (s)
	{
		XPoint p;
		float  amp;

		p.x = getIntOptionNamed (option, nOption, "x", s->width / 2);
		p.y = getIntOptionNamed (option, nOption, "y", s->height / 2);

		amp = getFloatOptionNamed (option, nOption, "amplitude", 0.5f);

		waterVertices (s, GL_POINTS, &p, 1, amp);

		damageScreen (s);
	}

	return FALSE;
}

static Bool
waterLine (BananaArgument     *arg,
           int                nArg)
{
	CompScreen *s;
	int        xid;

	xid = getIntOptionNamed (option, nOption, "root", 0);

	s = findScreenAtDisplay (d, xid);
	if (s)
	{
		XPoint p[2];
		float  amp;

		p[0].x = getIntOptionNamed (option, nOption, "x0", s->width / 4);
		p[0].y = getIntOptionNamed (option, nOption, "y0", s->height / 2);

		p[1].x = getIntOptionNamed (option, nOption, "x1",
		                        s->width - s->width / 4);
		p[1].y = getIntOptionNamed (option, nOption, "y1", s->height / 2);


		amp = getFloatOptionNamed (option, nOption, "amplitude", 0.25f);

		waterVertices (s, GL_LINES, p, 2, amp);

		damageScreen (s);
	}

	return FALSE;
}
*/

static void
waterHandleEvent (XEvent      *event)
{
	CompScreen *s;

	WATER_DISPLAY (&display);

	switch (event->type) {
	case KeyPress:
		if (isKeyPressEvent (event, &toggle_wiper_key))
		{
			BananaArgument arg;
			arg.name = "root";
			arg.type = BananaInt;
			arg.value.i = event->xkey.root;
			waterToggleWiper (&arg, 1);
		}
		else if (isKeyPressEvent (event, &toggle_rain_key))
		{
			BananaArgument arg;
			arg.name = "root";
			arg.type = BananaInt;
			arg.value.i = event->xkey.root;
			waterToggleRain (&arg, 1);
		}
		break;
	case ButtonPress:
		s = findScreenAtDisplay (event->xbutton.root);
		if (s)
		{
			WATER_SCREEN (s);

			if (ws->grabIndex)
			{
				XPoint p;

				p.x = pointerX;
				p.y = pointerY;

				waterVertices (s, GL_POINTS, &p, 1, 0.8f);
				damageScreen (s);
			}
		}
		break;
	case EnterNotify:
	case LeaveNotify:
		waterHandleMotionEvent (&display, event->xcrossing.root);
		break;
	case MotionNotify:
		waterHandleMotionEvent (&display, event->xmotion.root);
	default:
		if (event->type == display.xkbEvent)
		{
			XkbAnyEvent *xkbEvent = (XkbAnyEvent *) event;

			if (xkbEvent->xkb_type == XkbStateNotify)
			{
				XkbStateNotifyEvent *stateEvent = (XkbStateNotifyEvent *) event;

				if (stateEvent->event_type == KeyPress)
				{
					unsigned int    modMask = REAL_MOD_MASK & ~display.ignoredModMask;

					unsigned int bindMods = virtualToRealModMask ( 
					                           initiate_key_modifiers);

					if ((stateEvent->mods & modMask & bindMods) == bindMods)
					{
						waterInitiate (NULL, 0);
					}
				}
				else
				{
					unsigned int    modMask = REAL_MOD_MASK & ~display.ignoredModMask;

					unsigned int bindMods = virtualToRealModMask ( 
					                           initiate_key_modifiers);

					if ((stateEvent->mods & modMask & bindMods) != bindMods)
						waterTerminate (NULL, 0);
				}
			}
			else if (xkbEvent->xkb_type == XkbBellNotify)
			{
				const BananaValue *
				option_title_wave = bananaGetOption (bananaIndex,
				                                     "title_wave",
				                                     -1);

				if (option_title_wave->b)
				{
					BananaArgument arg;

					arg.name = "window";
					arg.type = BananaInt;
					arg.value.i = display.activeWindow;

					waterTitleWave (&arg, 1);
				}
			}
		}
	}

	UNWRAP (wd, &display, handleEvent);
	(*display.handleEvent) (event);
	WRAP (wd, &display, handleEvent, waterHandleEvent);
}



static Bool
waterInitDisplay (CompPlugin  *p,
                  CompDisplay *d)
{
	WaterDisplay *wd;

	wd = malloc (sizeof (WaterDisplay));
	if (!wd)
		return FALSE;

	wd->screenPrivateIndex = allocateScreenPrivateIndex ();
	if (wd->screenPrivateIndex < 0)
	{
		free (wd);
		return FALSE;
	}

	const BananaValue *
	option_offset_scale = bananaGetOption (bananaIndex,
	                                       "offset_scale",
	                                       -1);

	wd->offsetScale = option_offset_scale->f * 50.0f;

	WRAP (wd, d, handleEvent, waterHandleEvent);

	d->privates[displayPrivateIndex].ptr = wd;

	return TRUE;
}

static void
waterFiniDisplay (CompPlugin  *p,
                  CompDisplay *d)
{
	WATER_DISPLAY (d);

	freeScreenPrivateIndex (wd->screenPrivateIndex);

	UNWRAP (wd, d, handleEvent);

	free (wd);
}

static Bool
waterInitScreen (CompPlugin *p,
                 CompScreen *s)
{
	WaterScreen *ws;

	WATER_DISPLAY (&display);

	ws = calloc (1, sizeof (WaterScreen));
	if (!ws)
		return FALSE;

	ws->grabIndex = 0;

	WRAP (ws, s, preparePaintScreen, waterPreparePaintScreen);
	WRAP (ws, s, donePaintScreen, waterDonePaintScreen);
	WRAP (ws, s, drawWindowTexture, waterDrawWindowTexture);

	s->privates[wd->screenPrivateIndex].ptr = ws;

	waterReset (s);

	return TRUE;
}

static void
waterFiniScreen (CompPlugin *p,
                 CompScreen *s)
{
	WaterFunction *function, *next;
	int           i;

	WATER_SCREEN (s);

	if (ws->rainHandle)
		compRemoveTimeout (ws->rainHandle);

	if (ws->wiperHandle)
		compRemoveTimeout (ws->wiperHandle);

	if (ws->fbo)
		(*s->deleteFramebuffers) (1, &ws->fbo);

	for (i = 0; i < TEXTURE_NUM; i++)
	{
		if (ws->texture[i])
			glDeleteTextures (1, &ws->texture[i]);
	}

	if (ws->program)
		(*s->deletePrograms) (1, &ws->program);

	if (ws->data)
		free (ws->data);

	function = ws->bumpMapFunctions;
	while (function)
	{
		destroyFragmentFunction (s, function->handle);

		next = function->next;
		free (function);
		function = next;
	}

	UNWRAP (ws, s, preparePaintScreen);
	UNWRAP (ws, s, donePaintScreen);
	UNWRAP (ws, s, drawWindowTexture);

	free (ws);
}

static void
waterChangeNotify (const char        *optionName,
                   BananaType        optionType,
                   const BananaValue *optionValue,
                   int               screenNum)
{
	if (strcasecmp (optionName, "initiate_key") == 0)
		initiate_key_modifiers = stringToModifiers (optionValue->s);

	else if (strcasecmp (optionName, "toggle_rain_key") == 0)
		updateKey (optionValue->s, &toggle_rain_key);

	else if (strcasecmp (optionName, "toggle_wiper_key") == 0)
		updateKey (optionValue->s, &toggle_wiper_key);

	else if (strcasecmp (optionName, "offset_scale") == 0)
	{
		WATER_DISPLAY (&display);

		if (strcasecmp (optionName, "offset_scale") == 0)
			wd->offsetScale = optionValue->f * 50.0f;
	}

	else if (strcasecmp (optionName, "rain_delay") == 0)
	{
		CompScreen *s;

		for (s = display.screens; s; s = s->next)
		{
			WATER_SCREEN (s);

			if (!ws->rainHandle)
				continue;

			compRemoveTimeout (ws->rainHandle);
			ws->rainHandle = compAddTimeout (optionValue->i,
			                    (float)optionValue->i * 1.2,
			                    waterRainTimeout, s);
		}
	}
}

static Bool
waterInit (CompPlugin *p)
{
	if (getCoreABI() != CORE_ABIVERSION)
	{
		compLogMessage ("water", CompLogLevelError,
		                "ABI mismatch\n"
		                "\tPlugin was compiled with ABI: %d\n"
		                "\tFusilli Core was compiled with ABI: %d\n",
		                CORE_ABIVERSION, getCoreABI());

		return FALSE;
	}

	displayPrivateIndex = allocateDisplayPrivateIndex ();

	if (displayPrivateIndex < 0)
		return FALSE;

	bananaIndex = bananaLoadPlugin ("water");

	if (bananaIndex == -1)
		return FALSE;

	bananaAddChangeNotifyCallBack (bananaIndex, waterChangeNotify);

	const BananaValue *
	option_toggle_rain_key = bananaGetOption (bananaIndex,
	                                          "toggle_rain_key",
	                                          -1);

	const BananaValue *
	option_toggle_wiper_key = bananaGetOption (bananaIndex,
	                                           "toggle_wiper_key",
	                                           -1);

	registerKey (option_toggle_rain_key->s, &toggle_rain_key);
	registerKey (option_toggle_wiper_key->s, &toggle_wiper_key);

	//initiate key is not a passive grab key
	const BananaValue *
	option_initiate_key = bananaGetOption (bananaIndex,
	                                       "initiate_key",
	                                       -1);

	initiate_key_modifiers = stringToModifiers (option_initiate_key->s);

	return TRUE;
}

static void
waterFini (CompPlugin *p)
{
	freeDisplayPrivateIndex (displayPrivateIndex);

	bananaUnloadPlugin (bananaIndex);
}

static CompPluginVTable waterVTable = {
	"water",
	waterInit,
	waterFini,
	waterInitDisplay,
	waterFiniDisplay,
	waterInitScreen,
	waterFiniScreen,
	NULL, /* waterInitWindow */
	NULL  /* waterFiniWindow */
};

CompPluginVTable *
getCompPluginInfo20141205 (void)
{
	return &waterVTable;
}
