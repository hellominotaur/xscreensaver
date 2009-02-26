/* molecule, Copyright (c) 2001-2005 Jamie Zawinski <jwz@jwz.org>
 * Draws molecules, based on coordinates from PDB (Protein Data Base) files.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or 
 * implied warranty.
 */


/* Documentation on the PDB file format:
   http://www.rcsb.org/pdb/docs/format/pdbguide2.2/guide2.2_frame.html

   Good source of PDB files:
   http://www.sci.ouc.bc.ca/chem/molecule/molecule.html
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <X11/Intrinsic.h>

#define PROGCLASS	"Molecule"
#define HACK_INIT	init_molecule
#define HACK_DRAW	draw_molecule
#define HACK_RESHAPE	reshape_molecule
#define HACK_HANDLE_EVENT molecule_handle_event
#define EVENT_MASK	PointerMotionMask
#define molecule_opts	xlockmore_opts

#define DEF_TIMEOUT     "20"
#define DEF_SPIN        "XYZ"
#define DEF_WANDER      "False"
#define DEF_LABELS      "True"
#define DEF_TITLES      "True"
#define DEF_ATOMS       "True"
#define DEF_BONDS       "True"
#define DEF_SHELLS      "False"
#define DEF_BBOX        "False"
#define DEF_SHELL_ALPHA "0.3"
#define DEF_MOLECULE    "(default)"
#define DEF_VERBOSE     "False"

#define DEFAULTS	"*delay:	10000         \n" \
			"*showFPS:      False         \n" \
			"*wireframe:    False         \n" \
			"*atomFont:   -*-times-bold-r-normal-*-240-*\n" \
			"*titleFont:  -*-times-bold-r-normal-*-180-*\n" \
			"*noLabelThreshold:    30     \n" \
			"*wireframeThreshold:  150    \n" \


#undef countof
#define countof(x) (sizeof((x))/sizeof((*x)))

#include "xlockmore.h"
#include "colors.h"
#include "sphere.h"
#include "tube.h"
#include "glxfonts.h"
#include "rotator.h"
#include "gltrackball.h"

#ifdef USE_GL /* whole file */

#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <GL/glu.h>

#define SPHERE_SLICES 24  /* how densely to render spheres */
#define SPHERE_STACKS 12

#define SMOOTH_TUBE       /* whether to have smooth or faceted tubes */

#ifdef SMOOTH_TUBE
# define TUBE_FACES  12   /* how densely to render tubes */
#else
# define TUBE_FACES  8
#endif

static int scale_down;
#define SPHERE_SLICES_2  7
#define SPHERE_STACKS_2  4
#define TUBE_FACES_2     3


# ifdef __GNUC__
  __extension__  /* don't warn about "string length is greater than the length
                    ISO C89 compilers are required to support" when includng
                    the following data file... */
# endif
const char * const builtin_pdb_data[] = {
# include "molecules.h"
};


typedef struct {
  const char *name;
  GLfloat size, size2;
  const char *color;
  const char *text_color;
  GLfloat gl_color[8];
} atom_data;


/* These are the traditional colors used to render these atoms,
   and their approximate size in angstroms.
 */
static atom_data all_atom_data[] = {
  { "H",    1.17,  0,  "White",           "Grey70",        { 0, }},
  { "C",    1.75,  0,  "Grey60",          "White",         { 0, }},
  { "CA",   1.80,  0,  "Blue",            "LightBlue",     { 0, }},
  { "N",    1.55,  0,  "LightSteelBlue3", "SlateBlue1",    { 0, }},
  { "O",    1.40,  0,  "Red",             "LightPink",     { 0, }},
  { "P",    1.28,  0,  "MediumPurple",    "PaleVioletRed", { 0, }},
  { "S",    1.80,  0,  "Yellow4",         "Yellow1",       { 0, }},
  { "bond", 0,     0,  "Grey70",          "Yellow1",       { 0, }},
  { "*",    1.40,  0,  "Green4",          "LightGreen",    { 0, }}
};


typedef struct {
  int id;		/* sequence number in the PDB file */
  const char *label;	/* The atom name */
  GLfloat x, y, z;	/* position in 3-space (angstroms) */
  atom_data *data;	/* computed: which style of atom this is */
} molecule_atom;

typedef struct {
  int from, to;		/* atom sequence numbers */
  int strength;		/* how many bonds are between these two atoms */
} molecule_bond;


typedef struct {
  const char *label;		/* description of this compound */
  int natoms, atoms_size;
  int nbonds, bonds_size;
  molecule_atom *atoms;
  molecule_bond *bonds;
} molecule;


typedef struct {
  GLXContext *glx_context;
  rotator *rot;
  trackball_state *trackball;
  Bool button_down_p;

  GLfloat molecule_size;	   /* max dimension of molecule bounding box */

  GLfloat no_label_threshold;	   /* Things happen when molecules are huge */
  GLfloat wireframe_threshold;

  int which;			   /* which of the molecules is being shown */
  int nmolecules;
  molecule *molecules;

  int mode;  /* 0 = normal, 1 = out, 2 = in */
  int mode_tick;

  GLuint molecule_dlist;
  GLuint shell_dlist;

  XFontStruct *xfont1, *xfont2;
  GLuint font1_dlist, font2_dlist;
  int polygon_count;

} molecule_configuration;


static molecule_configuration *mcs = NULL;

static int timeout;
static char *molecule_str;
static char *do_spin;
static Bool do_wander;
static Bool do_titles;
static Bool do_labels;
static Bool do_atoms;
static Bool do_bonds;
static Bool do_shells;
static Bool do_bbox;
static Bool verbose_p;
static GLfloat shell_alpha;

/* saved to reset */
static Bool orig_do_labels, orig_do_atoms, orig_do_bonds, orig_do_shells,
    orig_wire;


static XrmOptionDescRec opts[] = {
  { "-molecule",	".molecule",	XrmoptionSepArg, 0 },
  { "-timeout",		".timeout",	XrmoptionSepArg, 0 },
  { "-spin",		".spin",	XrmoptionSepArg, 0 },
  { "+spin",		".spin",	XrmoptionNoArg, "" },
  { "-wander",		".wander",	XrmoptionNoArg, "True" },
  { "+wander",		".wander",	XrmoptionNoArg, "False" },
  { "-labels",		".labels",	XrmoptionNoArg, "True" },
  { "+labels",		".labels",	XrmoptionNoArg, "False" },
  { "-titles",		".titles",	XrmoptionNoArg, "True" },
  { "+titles",		".titles",	XrmoptionNoArg, "False" },
  { "-atoms",		".atoms",	XrmoptionNoArg, "True" },
  { "+atoms",		".atoms",	XrmoptionNoArg, "False" },
  { "-bonds",		".bonds",	XrmoptionNoArg, "True" },
  { "+bonds",		".bonds",	XrmoptionNoArg, "False" },
  { "-shells",		".eshells",	XrmoptionNoArg, "True" },
  { "+shells",		".eshells",	XrmoptionNoArg, "False" },
  { "-shell-alpha",	".shellAlpha",	XrmoptionSepArg, 0 },
  { "-bbox",		".bbox",	XrmoptionNoArg, "True" },
  { "+bbox",		".bbox",	XrmoptionNoArg, "False" },
  { "-verbose",		".verbose",	XrmoptionNoArg, "True" },
};

static argtype vars[] = {
  {&molecule_str, "molecule",	"Molecule",	DEF_MOLECULE, t_String},
  {&timeout,	  "timeout",	"Seconds",	DEF_TIMEOUT,  t_Int},
  {&do_spin,	  "spin",	"Spin",		DEF_SPIN,     t_String},
  {&do_wander,	  "wander",	"Wander",	DEF_WANDER,   t_Bool},
  {&do_atoms,	  "atoms",	"Atoms",	DEF_ATOMS,    t_Bool},
  {&do_bonds,	  "bonds",	"Bonds",	DEF_BONDS,    t_Bool},
  {&do_shells,	  "eshells",	"EShells",	DEF_SHELLS,   t_Bool},
  {&do_labels,	  "labels",	"Labels",	DEF_LABELS,   t_Bool},
  {&do_titles,	  "titles",	"Titles",	DEF_TITLES,   t_Bool},
  {&do_bbox,	  "bbox",	"BBox",		DEF_BBOX,     t_Bool},
  {&shell_alpha,  "shellAlpha",	"ShellAlpha",	DEF_SHELL_ALPHA, t_Float},
  {&verbose_p,	  "verbose",	"Verbose",	DEF_VERBOSE,  t_Bool},
};

ModeSpecOpt molecule_opts = {countof(opts), opts, countof(vars), vars, NULL};




/* shapes */

static int
sphere (GLfloat x, GLfloat y, GLfloat z, GLfloat diameter, Bool wire)
{
  int stacks = (scale_down ? SPHERE_STACKS_2 : SPHERE_STACKS);
  int slices = (scale_down ? SPHERE_SLICES_2 : SPHERE_SLICES);

  glPushMatrix ();
  glTranslatef (x, y, z);
  glScalef (diameter, diameter, diameter);
  unit_sphere (stacks, slices, wire);
  glPopMatrix ();

  return stacks * slices;
}


static void
load_fonts (ModeInfo *mi)
{
  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];
  load_font (mi->dpy, "atomFont",  &mc->xfont1, &mc->font1_dlist);
  load_font (mi->dpy, "titleFont", &mc->xfont2, &mc->font2_dlist);
}


static atom_data *
get_atom_data (const char *atom_name)
{
  int i;
  atom_data *d = 0;
  char *n = strdup (atom_name);
  char *n2 = n;
  int L;

  while (!isalpha(*n)) n++;
  L = strlen(n);
  while (L > 0 && !isalpha(n[L-1]))
    n[--L] = 0;

  for (i = 0; i < countof(all_atom_data); i++)
    {
      d = &all_atom_data[i];
      if (!strcasecmp (n, all_atom_data[i].name))
        break;
    }

  free (n2);
  return d;
}


static void
set_atom_color (ModeInfo *mi, molecule_atom *a, Bool font_p, GLfloat alpha)
{
  atom_data *d;
  GLfloat *gl_color;

  if (a)
    d = a->data;
  else
    {
      static atom_data *def_data = 0;
      if (!def_data) def_data = get_atom_data ("bond");
      d = def_data;
    }

  gl_color = (!font_p ? d->gl_color : (d->gl_color + 4));

  if (gl_color[3] == 0)
    {
      const char *string = !font_p ? d->color : d->text_color;
      XColor xcolor;
      if (!XParseColor (mi->dpy, mi->xgwa.colormap, string, &xcolor))
        {
          fprintf (stderr, "%s: unparsable color in %s: %s\n", progname,
                   (a ? a->label : d->name), string);
          exit (1);
        }

      gl_color[0] = xcolor.red   / 65536.0;
      gl_color[1] = xcolor.green / 65536.0;
      gl_color[2] = xcolor.blue  / 65536.0;
    }
  
  gl_color[3] = alpha;

  if (font_p)
    glColor4f (gl_color[0], gl_color[1], gl_color[2], gl_color[3]);
  else
    glMaterialfv (GL_FRONT, GL_AMBIENT_AND_DIFFUSE, gl_color);
}


static GLfloat
atom_size (molecule_atom *a)
{
  if (do_bonds)
    {
      if (a->data->size2 == 0)
        {
          /* let the molecules have the same relative sizes, but scale
             them to a smaller range, so that the bond-tubes are
             actually visible...
           */
          GLfloat bot = 0.4;
          GLfloat top = 0.6;
          GLfloat min = 1.17;
          GLfloat max = 1.80;
          GLfloat ratio = (a->data->size - min) / (max - min);
          a->data->size2 = bot + (ratio * (top - bot));
        }
      return a->data->size2;
    }
  else
    return a->data->size;
}


static molecule_atom *
get_atom (molecule_atom *atoms, int natoms, int id)
{
  int i;

  /* quick short-circuit */
  if (id < natoms)
    {
      if (atoms[id].id == id)
        return &atoms[id];
      if (id > 0 && atoms[id-1].id == id)
        return &atoms[id-1];
      if (id < natoms-1 && atoms[id+1].id == id)
        return &atoms[id+1];
    }

  for (i = 0; i < natoms; i++)
    if (id == atoms[i].id)
      return &atoms[i];

  fprintf (stderr, "%s: no atom %d\n", progname, id);
  abort();
}


static void
molecule_bounding_box (ModeInfo *mi,
                       GLfloat *x1, GLfloat *y1, GLfloat *z1,
                       GLfloat *x2, GLfloat *y2, GLfloat *z2)
{
  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];
  molecule *m = &mc->molecules[mc->which];
  int i;

  if (m->natoms == 0)
    {
      *x1 = *y1 = *z1 = *x2 = *y2 = *z2 = 0;
    }
  else
    {
      *x1 = *x2 = m->atoms[0].x;
      *y1 = *y2 = m->atoms[0].y;
      *z1 = *z2 = m->atoms[0].z;
    }

  for (i = 1; i < m->natoms; i++)
    {
      if (m->atoms[i].x < *x1) *x1 = m->atoms[i].x;
      if (m->atoms[i].y < *y1) *y1 = m->atoms[i].y;
      if (m->atoms[i].z < *z1) *z1 = m->atoms[i].z;

      if (m->atoms[i].x > *x2) *x2 = m->atoms[i].x;
      if (m->atoms[i].y > *y2) *y2 = m->atoms[i].y;
      if (m->atoms[i].z > *z2) *z2 = m->atoms[i].z;
    }

  *x1 -= 1.5;
  *y1 -= 1.5;
  *z1 -= 1.5;
  *x2 += 1.5;
  *y2 += 1.5;
  *z2 += 1.5;
}


static void
draw_bounding_box (ModeInfo *mi)
{
  static GLfloat c1[4] = { 0.2, 0.2, 0.4, 1.0 };
  static GLfloat c2[4] = { 1.0, 0.0, 0.0, 1.0 };
  int wire = MI_IS_WIREFRAME(mi);
  GLfloat x1, y1, z1, x2, y2, z2;
  molecule_bounding_box (mi, &x1, &y1, &z1, &x2, &y2, &z2);

  glMaterialfv (GL_FRONT, GL_AMBIENT_AND_DIFFUSE, c1);
  glFrontFace(GL_CCW);

  glBegin(wire ? GL_LINE_LOOP : GL_QUADS);
  glNormal3f(0, 1, 0);
  glVertex3f(x1, y1, z1); glVertex3f(x1, y1, z2);
  glVertex3f(x2, y1, z2); glVertex3f(x2, y1, z1);
  glEnd();
  glBegin(wire ? GL_LINE_LOOP : GL_QUADS);
  glNormal3f(0, -1, 0);
  glVertex3f(x2, y2, z1); glVertex3f(x2, y2, z2);
  glVertex3f(x1, y2, z2); glVertex3f(x1, y2, z1);
  glEnd();
  glBegin(wire ? GL_LINE_LOOP : GL_QUADS);
  glNormal3f(0, 0, 1);
  glVertex3f(x1, y1, z1); glVertex3f(x2, y1, z1);
  glVertex3f(x2, y2, z1); glVertex3f(x1, y2, z1);
  glEnd();
  glBegin(wire ? GL_LINE_LOOP : GL_QUADS);
  glNormal3f(0, 0, -1);
  glVertex3f(x1, y2, z2); glVertex3f(x2, y2, z2);
  glVertex3f(x2, y1, z2); glVertex3f(x1, y1, z2);
  glEnd();
  glBegin(wire ? GL_LINE_LOOP : GL_QUADS);
  glNormal3f(1, 0, 0);
  glVertex3f(x1, y2, z1); glVertex3f(x1, y2, z2);
  glVertex3f(x1, y1, z2); glVertex3f(x1, y1, z1);
  glEnd();
  glBegin(wire ? GL_LINE_LOOP : GL_QUADS);
  glNormal3f(-1, 0, 0);
  glVertex3f(x2, y1, z1); glVertex3f(x2, y1, z2);
  glVertex3f(x2, y2, z2); glVertex3f(x2, y2, z1);
  glEnd();

  glPushAttrib (GL_LIGHTING);
  glDisable (GL_LIGHTING);

  glColor3f (c2[0], c2[1], c2[2]);
  glBegin(GL_LINES);
  if (x1 > 0) x1 = 0; if (x2 < 0) x2 = 0;
  if (y1 > 0) y1 = 0; if (y2 < 0) y2 = 0;
  if (z1 > 0) z1 = 0; if (z2 < 0) z2 = 0;
  glVertex3f(x1, 0,  0);  glVertex3f(x2, 0,  0); 
  glVertex3f(0 , y1, 0);  glVertex3f(0,  y2, 0); 
  glVertex3f(0,  0,  z1); glVertex3f(0,  0,  z2); 
  glEnd();

  glPopAttrib();
}


/* Since PDB files don't always have the molecule centered around the
   origin, and since some molecules are pretty large, scale and/or
   translate so that the whole molecule is visible in the window.
 */
static void
ensure_bounding_box_visible (ModeInfo *mi)
{
  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];

  GLfloat x1, y1, z1, x2, y2, z2;
  GLfloat w, h, d;
  GLfloat size;
  GLfloat max_size = 10;  /* don't bother scaling down if the molecule
                             is already smaller than this */

  molecule_bounding_box (mi, &x1, &y1, &z1, &x2, &y2, &z2);
  w = x2-x1;
  h = y2-y1;
  d = z2-z1;

  size = (w > h ? w : h);
  size = (size > d ? size : d);

  mc->molecule_size = size;

  scale_down = 0;

  if (size > max_size)
    {
      GLfloat scale = max_size / size;
      glScalef (scale, scale, scale);

      scale_down = scale < 0.3;
    }

  glTranslatef (-(x1 + w/2),
                -(y1 + h/2),
                -(z1 + d/2));
}



/* Constructs the GL shapes of the current molecule
 */
static void
build_molecule (ModeInfo *mi, Bool transparent_p)
{
  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];
  int wire = MI_IS_WIREFRAME(mi);
  int i;
  GLfloat alpha = transparent_p ? shell_alpha : 1.0;
  int polys = 0;

  molecule *m = &mc->molecules[mc->which];

  if (wire)
    {
      glDisable(GL_CULL_FACE);
      glDisable(GL_LIGHTING);
      glDisable(GL_LIGHT0);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_NORMALIZE);
      glDisable(GL_CULL_FACE);
    }
  else
    {
      glEnable(GL_CULL_FACE);
      glEnable(GL_LIGHTING);
      glEnable(GL_LIGHT0);
      glEnable(GL_DEPTH_TEST);
      glEnable(GL_NORMALIZE);
      glEnable(GL_CULL_FACE);
    }

  if (!wire)
    set_atom_color (mi, 0, False, alpha);

  if (do_bonds)
    for (i = 0; i < m->nbonds; i++)
      {
        molecule_bond *b = &m->bonds[i];
        molecule_atom *from = get_atom (m->atoms, m->natoms, b->from);
        molecule_atom *to   = get_atom (m->atoms, m->natoms, b->to);

        if (wire)
          {
            glBegin(GL_LINES);
            glVertex3f(from->x, from->y, from->z);
            glVertex3f(to->x,   to->y,   to->z);
            glEnd();
            polys++;
          }
        else
          {
            int faces = (scale_down ? TUBE_FACES_2 : TUBE_FACES);
# ifdef SMOOTH_TUBE
            int smooth = True;
# else
            int smooth = False;
# endif
            GLfloat thickness = 0.07 * b->strength;
            GLfloat cap_size = 0.03;
            if (thickness > 0.3)
              thickness = 0.3;

            tube (from->x, from->y, from->z,
                  to->x,   to->y,   to->z,
                  thickness, cap_size,
                  faces, smooth, (!do_atoms || do_shells), wire);
            polys += faces;
          }
      }

  if (!wire && do_atoms)
    for (i = 0; i < m->natoms; i++)
      {
        molecule_atom *a = &m->atoms[i];
        GLfloat size = atom_size (a);
        set_atom_color (mi, a, False, alpha);
        polys += sphere (a->x, a->y, a->z, size, wire);
      }

  if (do_bbox && !transparent_p)
    {
      draw_bounding_box (mi);
      polys += 4;
    }

  mc->polygon_count += polys;
}



/* loading */

static void
push_atom (molecule *m,
           int id, const char *label,
           GLfloat x, GLfloat y, GLfloat z)
{
  m->natoms++;
  if (m->atoms_size < m->natoms)
    {
      m->atoms_size += 20;
      m->atoms = (molecule_atom *) realloc (m->atoms,
                                            m->atoms_size * sizeof(*m->atoms));
    }
  m->atoms[m->natoms-1].id = id;
  m->atoms[m->natoms-1].label = label;
  m->atoms[m->natoms-1].x = x;
  m->atoms[m->natoms-1].y = y;
  m->atoms[m->natoms-1].z = z;
  m->atoms[m->natoms-1].data = get_atom_data (label);
}


static void
push_bond (molecule *m, int from, int to)
{
  int i;

  for (i = 0; i < m->nbonds; i++)
    if ((m->bonds[i].from == from && m->bonds[i].to   == to) ||
        (m->bonds[i].to   == from && m->bonds[i].from == to))
      {
        m->bonds[i].strength++;
        return;
      }

  m->nbonds++;
  if (m->bonds_size < m->nbonds)
    {
      m->bonds_size += 20;
      m->bonds = (molecule_bond *) realloc (m->bonds,
                                            m->bonds_size * sizeof(*m->bonds));
    }
  m->bonds[m->nbonds-1].from = from;
  m->bonds[m->nbonds-1].to = to;
  m->bonds[m->nbonds-1].strength = 1;
}



/* This function is crap.
 */
static void
parse_pdb_data (molecule *m, const char *data, const char *filename, int line)
{
  const char *s = data;
  char *ss;
  while (*s)
    {
      if ((!m->label || !*m->label) &&
          (!strncmp (s, "HEADER", 6) || !strncmp (s, "COMPND", 6)))
        {
          char *name = calloc (1, 100);
          char *n2 = name;
          int L = strlen(s);
          if (L > 99) L = 99;

          strncpy (n2, s, L);
          n2 += 7;
          while (isspace(*n2)) n2++;

          ss = strchr (n2, '\n');
          if (ss) *ss = 0;
          ss = strchr (n2, '\r');
          if (ss) *ss = 0;

          ss = n2+strlen(n2)-1;
          while (isspace(*ss) && ss > n2)
            *ss-- = 0;

          if (strlen (n2) > 4 &&
              !strcmp (n2 + strlen(n2) - 4, ".pdb"))
            n2[strlen(n2)-4] = 0;

          if (m->label) free ((char *) m->label);
          m->label = strdup (n2);
          free (name);
        }
      else if (!strncmp (s, "TITLE ", 6) ||
               !strncmp (s, "HEADER", 6) ||
               !strncmp (s, "COMPND", 6) ||
               !strncmp (s, "AUTHOR", 6) ||
               !strncmp (s, "REVDAT", 6) ||
               !strncmp (s, "SOURCE", 6) ||
               !strncmp (s, "EXPDTA", 6) ||
               !strncmp (s, "JRNL  ", 6) ||
               !strncmp (s, "REMARK", 6) ||
               !strncmp (s, "SEQRES", 6) ||
               !strncmp (s, "HET   ", 6) ||
               !strncmp (s, "FORMUL", 6) ||
               !strncmp (s, "CRYST1", 6) ||
               !strncmp (s, "ORIGX1", 6) ||
               !strncmp (s, "ORIGX2", 6) ||
               !strncmp (s, "ORIGX3", 6) ||
               !strncmp (s, "SCALE1", 6) ||
               !strncmp (s, "SCALE2", 6) ||
               !strncmp (s, "SCALE3", 6) ||
               !strncmp (s, "MASTER", 6) ||
               !strncmp (s, "KEYWDS", 6) ||
               !strncmp (s, "DBREF ", 6) ||
               !strncmp (s, "HETNAM", 6) ||
               !strncmp (s, "HETSYN", 6) ||
               !strncmp (s, "HELIX ", 6) ||
               !strncmp (s, "LINK  ", 6) ||
               !strncmp (s, "MTRIX1", 6) ||
               !strncmp (s, "MTRIX2", 6) ||
               !strncmp (s, "MTRIX3", 6) ||
               !strncmp (s, "SHEET ", 6) ||
               !strncmp (s, "CISPEP", 6) ||
               !strncmp (s, "GENERATED BY", 12) ||
               !strncmp (s, "TER ", 4) ||
               !strncmp (s, "END ", 4) ||
               !strncmp (s, "TER\n", 4) ||
               !strncmp (s, "END\n", 4) ||
               !strncmp (s, "\n", 1))
        /* ignored. */
        ;
      else if (!strncmp (s, "ATOM   ", 7))
        {
          int id;
          char *name = (char *) calloc (1, 4);
          GLfloat x = -999, y = -999, z = -999;

          sscanf (s+7, " %d ", &id);

          strncpy (name, s+12, 3);
          while (isspace(*name)) name++;
          ss = name + strlen(name)-1;
          while (isspace(*ss) && ss > name)
            *ss-- = 0;
	  ss = name + 1;
	  while(*ss)
          {
	    *ss = tolower(*ss);
            ss++;
          }
          sscanf (s + 32, " %f %f %f ", &x, &y, &z);
/*
          fprintf (stderr, "%s: %s: %d: atom: %d \"%s\" %9.4f %9.4f %9.4f\n",
                   progname, filename, line,
                   id, name, x, y, z);
*/
          push_atom (m, id, name, x, y, z);
        }
      else if (!strncmp (s, "HETATM ", 7))
        {
          int id;
          char *name = (char *) calloc (1, 4);
          GLfloat x = -999, y = -999, z = -999;

          sscanf (s+7, " %d ", &id);

          strncpy (name, s+12, 3);
          while (isspace(*name)) name++;
          ss = name + strlen(name)-1;
          while (isspace(*ss) && ss > name)
            *ss-- = 0;
          sscanf (s + 30, " %f %f %f ", &x, &y, &z);
/*
          fprintf (stderr, "%s: %s: %d: atom: %d \"%s\" %9.4f %9.4f %9.4f\n",
                   progname, filename, line,
                   id, name, x, y, z);
*/
          push_atom (m, id, name, x, y, z);
        }
      else if (!strncmp (s, "CONECT ", 7))
        {
          int atoms[11];
          int i = sscanf (s + 8, " %d %d %d %d %d %d %d %d %d %d %d %d ",
                          &atoms[0], &atoms[1], &atoms[2], &atoms[3], 
                          &atoms[4], &atoms[5], &atoms[6], &atoms[7], 
                          &atoms[8], &atoms[9], &atoms[10], &atoms[11]);
          int j;
          for (j = 1; j < i; j++)
            if (atoms[j] > 0)
              {
/*
                fprintf (stderr, "%s: %s: %d: bond: %d %d\n",
                         progname, filename, line, atoms[0], atoms[j]);
*/
                push_bond (m, atoms[0], atoms[j]);
              }
        }
      else
        {
          char *s1 = strdup (s);
          for (ss = s1; *ss && *ss != '\n'; ss++)
            ;
          *ss = 0;
          fprintf (stderr, "%s: %s: %d: unrecognised line: %s\n",
                   progname, filename, line, s1);
        }

      while (*s && *s != '\n')
        s++;
      if (*s == '\n')
        s++;
      line++;
    }
}


static int
parse_pdb_file (molecule *m, const char *name)
{
  FILE *in;
  int buf_size = 40960;
  char *buf;
  int line = 1;

  in = fopen(name, "r");
  if (!in)
    {
      char *buf = (char *) malloc(1024 + strlen(name));
      sprintf(buf, "%s: error reading \"%s\"", progname, name);
      perror(buf);
      return -1;
    }

  buf = (char *) malloc (buf_size);

  while (fgets (buf, buf_size-1, in))
    {
      char *s;
      for (s = buf; *s; s++)
        if (*s == '\r') *s = '\n';
      parse_pdb_data (m, buf, name, line++);
    }

  free (buf);
  fclose (in);

  if (!m->natoms)
    {
      fprintf (stderr, "%s: file %s contains no atomic coordinates!\n",
               progname, name);
      return -1;
    }

  if (!m->nbonds && do_bonds)
    {
      fprintf (stderr, "%s: warning: file %s contains no atomic bond info.\n",
               progname, name);
      do_bonds = 0;
    }

  return 0;
}


typedef struct { char *atom; int count; } atom_and_count;

/* When listing the components of a molecule, the convention is to put the
   carbon atoms first, the hydrogen atoms second, and the other atom types
   sorted alphabetically after that (although for some molecules, the usual
   order is different: we special-case a few of those.)
 */
static int
cmp_atoms (const void *aa, const void *bb)
{
  const atom_and_count *a = (atom_and_count *) aa;
  const atom_and_count *b = (atom_and_count *) bb;
  if (!a->atom) return  1;
  if (!b->atom) return -1;
  if (!strcmp(a->atom, "C")) return -1;
  if (!strcmp(b->atom, "C")) return  1;
  if (!strcmp(a->atom, "H")) return -1;
  if (!strcmp(b->atom, "H")) return  1;
  return strcmp (a->atom, b->atom);
}

static void special_case_formula (char *f);

static void
generate_molecule_formula (molecule *m)
{
  char *buf = (char *) malloc (m->natoms * 10);
  char *s = buf;
  int i;
  atom_and_count counts[200];
  memset (counts, 0, sizeof(counts));
  *s = 0;
  for (i = 0; i < m->natoms; i++)
    {
      int j = 0;
      char *a = (char *) m->atoms[i].label;
      char *e;
      while (!isalpha(*a)) a++;
      a = strdup (a);
      for (e = a; isalpha(*e); e++);
      *e = 0;
      while (counts[j].atom && !!strcmp(a, counts[j].atom))
        j++;
      if (counts[j].atom)
        free (a);
      else
        counts[j].atom = a;
      counts[j].count++;
    }

  i = 0;
  while (counts[i].atom) i++;
  qsort (counts, i, sizeof(*counts), cmp_atoms);

  i = 0;
  while (counts[i].atom)
    {
      strcat (s, counts[i].atom);
      free (counts[i].atom);
      s += strlen (s);
      if (counts[i].count > 1)
        sprintf (s, "[%d]", counts[i].count);  /* use [] to get subscripts */
      s += strlen (s);
      i++;
    }

  special_case_formula (buf);

  if (!m->label) m->label = strdup("");
  s = (char *) malloc (strlen (m->label) + strlen (buf) + 2);
  strcpy (s, m->label);
  strcat (s, "\n");
  strcat (s, buf);
  free ((char *) m->label);
  free (buf);
  m->label = s;
}

/* thanks to Rene Uittenbogaard <ruittenb@wish.nl> */
static void
special_case_formula (char *f)
{
  if      (!strcmp(f, "H[2]Be"))   strcpy(f, "BeH[2]");
  else if (!strcmp(f, "H[3]B"))    strcpy(f, "BH[3]");
  else if (!strcmp(f, "H[3]N"))    strcpy(f, "NH[3]");
  else if (!strcmp(f, "CHN"))      strcpy(f, "HCN");
  else if (!strcmp(f, "CKN"))      strcpy(f, "KCN");
  else if (!strcmp(f, "H[4]N[2]")) strcpy(f, "N[2]H[4]");
  else if (!strcmp(f, "Cl[3]P"))   strcpy(f, "PCl[3]");
  else if (!strcmp(f, "Cl[5]P"))   strcpy(f, "PCl[5]");
}


static void
insert_vertical_whitespace (char *string)
{
  while (*string)
    {
      if ((string[0] == ',' ||
           string[0] == ';' ||
           string[0] == ':') &&
          string[1] == ' ')
        string[0] = ' ', string[1] = '\n';
      string++;
    }
}


/* Construct the molecule data from either: the builtins; or from
   the (one) .pdb file specified with -molecule.
 */
static void
load_molecules (ModeInfo *mi)
{
  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];
  int wire = MI_IS_WIREFRAME(mi);
  int i;

  mc->nmolecules = 0;
  if (molecule_str && *molecule_str && 
      strcmp(molecule_str, "(default)"))	/* try external PDB files */
    {
      /* The -molecule option can point to a .pdb file, or to
         a directory of them.
      */
      struct stat st;
      int nfiles = 0;
      int list_size = 0;
      char **files = 0;
      int molecule_ctr;

      if (!stat (molecule_str, &st) &&
          S_ISDIR (st.st_mode))
        {
          char buf [255];
          DIR *pdb_dir;
          struct dirent *dentry;

          pdb_dir = opendir (molecule_str);
          if (! pdb_dir)
            {
              sprintf (buf, "%.100s: %.100s", progname, molecule_str);
              perror (buf);
              exit (1);
            }

          if (verbose_p)
            fprintf (stderr, "%s: directory %s\n", progname, molecule_str);

          nfiles = 0;
          list_size = 100;
          files = (char **) calloc (sizeof(*files), list_size);

          while ((dentry = readdir (pdb_dir)))
            {
              int L = strlen (dentry->d_name);
              if (L > 4 && !strcasecmp (dentry->d_name + L - 4, ".pdb"))
                {
                  char *fn;
                  if (nfiles >= list_size-1)
                    {
                      list_size = (list_size + 10) * 1.2;
                      files = (char **)
                        realloc (files, list_size * sizeof(*files));
                      if (!files)
                        {
                        OOM:
                          fprintf (stderr, "%s: out of memory (%d files)\n",
                                   progname, nfiles);
                          exit (1);
                        }
                    }

                  fn = (char *) malloc (strlen (molecule_str) + L + 10);
                  if (!fn) goto OOM;
                  strcpy (fn, molecule_str);
                  if (fn[strlen(fn)-1] != '/') strcat (fn, "/");
                  strcat (fn, dentry->d_name);
                  files[nfiles++] = fn;
                  if (verbose_p)
                    fprintf (stderr, "%s: file %s\n", progname, fn);
                }
            }
          closedir (pdb_dir);

          if (nfiles == 0)
            fprintf (stderr, "%s: no .pdb files in directory %s\n",
                     progname, molecule_str);
        }
      else
        {
          files = (char **) malloc (sizeof (*files));
          nfiles = 1;
          files[0] = strdup (molecule_str);
          if (verbose_p)
            fprintf (stderr, "%s: file %s\n", progname, molecule_str);
        }

      mc->nmolecules = nfiles;
      mc->molecules = (molecule *) calloc (sizeof (molecule), mc->nmolecules);
      molecule_ctr = 0;
      for (i = 0; i < mc->nmolecules; i++)
        {
          if (verbose_p)
            fprintf (stderr, "%s: reading %s\n", progname, files[i]);
          if (!parse_pdb_file (&mc->molecules[molecule_ctr], files[i]))
            {
              if ((wire || !do_atoms) &&
                  !do_labels &&
                  mc->molecules[molecule_ctr].nbonds == 0)
                {
                  /* If we're not drawing atoms (e.g., wireframe mode), and
                     there is no bond info, then make sure labels are turned
                     on, or we'll be looking at a black screen... */
                  fprintf (stderr, "%s: %s: no bonds: turning -label on.\n",
                           progname, files[i]);
                  do_labels = 1;
                }
              free (files[i]);
	      files[i] = 0;
              molecule_ctr++;
	    }
        }

      free (files);
      files = 0;
      mc->nmolecules = molecule_ctr;
    }

  if (mc->nmolecules == 0)	/* do the builtins if no files */
    {
      mc->nmolecules = countof(builtin_pdb_data);
      mc->molecules = (molecule *) calloc (sizeof (molecule), mc->nmolecules);
      for (i = 0; i < mc->nmolecules; i++)
        {
          char name[100];
          sprintf (name, "<builtin-%d>", i);
          if (verbose_p) fprintf (stderr, "%s: reading %s\n", progname, name);
          parse_pdb_data (&mc->molecules[i], builtin_pdb_data[i], name, 1);
        }
    }

  for (i = 0; i < mc->nmolecules; i++)
    {
      generate_molecule_formula (&mc->molecules[i]);
      insert_vertical_whitespace ((char *) mc->molecules[i].label);
    }
}



/* Window management, etc
 */
void
reshape_molecule (ModeInfo *mi, int width, int height)
{
  GLfloat h = (GLfloat) height / (GLfloat) width;

  glViewport (0, 0, (GLint) width, (GLint) height);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective (30.0, 1/h, 20.0, 100.0);

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  gluLookAt( 0.0, 0.0, 30.0,
             0.0, 0.0, 0.0,
             0.0, 1.0, 0.0);

  glClear(GL_COLOR_BUFFER_BIT);
}


static void
gl_init (ModeInfo *mi)
{
  static GLfloat pos[4] = {1.0, 0.4, 0.9, 0.0};
  static GLfloat amb[4] = {0.0, 0.0, 0.0, 1.0};
  static GLfloat dif[4] = {0.8, 0.8, 0.8, 1.0};
  static GLfloat spc[4] = {1.0, 1.0, 1.0, 1.0};
  glLightfv(GL_LIGHT0, GL_POSITION, pos);
  glLightfv(GL_LIGHT0, GL_AMBIENT,  amb);
  glLightfv(GL_LIGHT0, GL_DIFFUSE,  dif);
  glLightfv(GL_LIGHT0, GL_SPECULAR, spc);
}


static void
startup_blurb (ModeInfo *mi)
{
  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];
  const char *s = "Constructing molecules...";
  print_gl_string (mi->dpy, mc->xfont2, mc->font2_dlist,
                   mi->xgwa.width, mi->xgwa.height,
                   10, mi->xgwa.height - 10,
                   s);
  glFinish();
  glXSwapBuffers(MI_DISPLAY(mi), MI_WINDOW(mi));
}

Bool
molecule_handle_event (ModeInfo *mi, XEvent *event)
{
  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];

  if (event->xany.type == ButtonPress &&
      event->xbutton.button == Button1)
    {
      mc->button_down_p = True;
      gltrackball_start (mc->trackball,
                         event->xbutton.x, event->xbutton.y,
                         MI_WIDTH (mi), MI_HEIGHT (mi));
      return True;
    }
  else if (event->xany.type == ButtonRelease &&
           event->xbutton.button == Button1)
    {
      mc->button_down_p = False;
      return True;
    }
  else if (event->xany.type == ButtonPress &&
           (event->xbutton.button == Button4 ||
            event->xbutton.button == Button5))
    {
      gltrackball_mousewheel (mc->trackball, event->xbutton.button, 10,
                              !!event->xbutton.state);
      return True;
    }
  else if (event->xany.type == KeyPress)
    {
      KeySym keysym;
      char c = 0;
      XLookupString (&event->xkey, &c, 1, &keysym, 0);

      if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
          GLfloat speed = 4.0;
          mc->mode = 1;
          mc->mode_tick = 10 * speed;
          return True;
        }
    }
  else if (event->xany.type == MotionNotify &&
           mc->button_down_p)
    {
      gltrackball_track (mc->trackball,
                         event->xmotion.x, event->xmotion.y,
                         MI_WIDTH (mi), MI_HEIGHT (mi));
      return True;
    }

  return False;
}


void 
init_molecule (ModeInfo *mi)
{
  molecule_configuration *mc;
  int wire;

  if (!mcs) {
    mcs = (molecule_configuration *)
      calloc (MI_NUM_SCREENS(mi), sizeof (molecule_configuration));
    if (!mcs) {
      fprintf(stderr, "%s: out of memory\n", progname);
      exit(1);
    }
  }

  mc = &mcs[MI_SCREEN(mi)];

  if ((mc->glx_context = init_GL(mi)) != NULL) {
    gl_init(mi);
    reshape_molecule (mi, MI_WIDTH(mi), MI_HEIGHT(mi));
  }

  load_fonts (mi);
  startup_blurb (mi);

  wire = MI_IS_WIREFRAME(mi);

  {
    Bool spinx=False, spiny=False, spinz=False;
    double spin_speed   = 0.5;
    double spin_accel   = 0.3;
    double wander_speed = 0.01;

    char *s = do_spin;
    while (*s)
      {
        if      (*s == 'x' || *s == 'X') spinx = True;
        else if (*s == 'y' || *s == 'Y') spiny = True;
        else if (*s == 'z' || *s == 'Z') spinz = True;
        else
          {
            fprintf (stderr,
         "%s: spin must contain only the characters X, Y, or Z (not \"%s\")\n",
                     progname, do_spin);
            exit (1);
          }
        s++;
      }

    mc->rot = make_rotator (spinx ? spin_speed : 0,
                            spiny ? spin_speed : 0,
                            spinz ? spin_speed : 0,
                            spin_accel,
                            do_wander ? wander_speed : 0,
                            (spinx && spiny && spinz));
    mc->trackball = gltrackball_init ();
  }

  orig_do_labels = do_labels;
  orig_do_atoms  = do_atoms;
  orig_do_bonds  = do_bonds;
  orig_do_shells = do_shells;
  orig_wire = MI_IS_WIREFRAME(mi);

  mc->molecule_dlist = glGenLists(1);
  if (do_shells)
    mc->shell_dlist = glGenLists(1);

  load_molecules (mi);
  mc->which = random() % mc->nmolecules;

  mc->no_label_threshold = get_float_resource ("noLabelThreshold",
                                               "NoLabelThreshold");
  mc->wireframe_threshold = get_float_resource ("wireframeThreshold",
                                                "WireframeThreshold");
  mc->mode = 0;

  if (wire)
    do_bonds = 1;
}


/* Put the labels on the atoms.
   This can't be a part of the display list because of the games
   we play with the translation matrix.
 */
void
draw_labels (ModeInfo *mi)
{
  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];
  int wire = MI_IS_WIREFRAME(mi);
  molecule *m = &mc->molecules[mc->which];
  int i, j;

  if (!do_labels)
    return;

  if (!wire)
    glDisable (GL_LIGHTING);   /* don't light fonts */

  for (i = 0; i < m->natoms; i++)
    {
      molecule_atom *a = &m->atoms[i];
      GLfloat size = atom_size (a);
      GLfloat m[4][4];

      glPushMatrix();

      if (!wire)
        set_atom_color (mi, a, True, 1);

      /* First, we translate the origin to the center of the atom.

         Then we retrieve the prevailing modelview matrix (which
         includes any rotation, wandering, and user-trackball-rolling
         of the scene.

         We set the top 3x3 cells of that matrix to be the identity
         matrix.  This removes all rotation from the matrix, while
         leaving the translation alone.  This has the effect of
         leaving the prevailing coordinate system perpendicular to
         the camera view: were we to draw a square face, it would
         be in the plane of the screen.

         Now we translate by `size' toward the viewer -- so that the
         origin is *just in front* of the ball.

         Then we draw the label text, allowing the depth buffer to
         do its work: that way, labels on atoms will be occluded
         properly when other atoms move in front of them.

         This technique (of neutralizing rotation relative to the
         observer, after both rotations and translations have been
         applied) is known as "billboarding".
       */

      glTranslatef(a->x, a->y, a->z);               /* get matrix */
      glGetFloatv (GL_MODELVIEW_MATRIX, &m[0][0]);  /* load rot. identity */
      m[0][0] = 1; m[1][0] = 0; m[2][0] = 0;
      m[0][1] = 0; m[1][1] = 1; m[2][1] = 0;
      m[0][2] = 0; m[1][2] = 0; m[2][2] = 1;
      glLoadIdentity();                             /* reset modelview */
      glMultMatrixf (&m[0][0]);                     /* replace with ours */

      glTranslatef (0, 0, (size * 1.1));           /* move toward camera */

      glRasterPos3f (0, 0, 0);                     /* draw text here */

      /* Before drawing the string, shift the origin to center
         the text over the origin of the sphere. */
      glBitmap (0, 0, 0, 0,
                -string_width (mc->xfont1, a->label) / 2,
                -mc->xfont1->descent,
                NULL);

      for (j = 0; j < strlen(a->label); j++)
        glCallList (mc->font1_dlist + (int)(a->label[j]));

      glPopMatrix();
    }

  /* More efficient to always call glEnable() with correct values
     than to call glPushAttrib()/glPopAttrib(), since reading
     attributes from GL does a round-trip and  stalls the pipeline.
   */
  if (!wire)
    glEnable (GL_LIGHTING);
}


static void
pick_new_molecule (ModeInfo *mi, time_t last)
{
  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];

  if (mc->nmolecules == 1)
    {
      if (last != 0) return;
      mc->which = 0;
    }
  else if (last == 0)
    {
      mc->which = random() % mc->nmolecules;
    }
  else
    {
      int n = mc->which;
      while (n == mc->which)
        n = random() % mc->nmolecules;
      mc->which = n;
    }
          
  if (verbose_p)
    {
      char *name = strdup (mc->molecules[mc->which].label);
      char *s = strpbrk (name, "\r\n");
      if (s) *s = 0;
      fprintf (stderr, "%s: drawing %s (%d)\n", progname, name, mc->which);
      free (name);
    }

  mc->polygon_count = 0;

  glNewList (mc->molecule_dlist, GL_COMPILE);
  ensure_bounding_box_visible (mi);

  do_labels = orig_do_labels;
  do_atoms  = orig_do_atoms;
  do_bonds  = orig_do_bonds;
  do_shells = orig_do_shells;
  MI_IS_WIREFRAME(mi) = orig_wire;

  if (mc->molecule_size > mc->no_label_threshold)
    do_labels = 0;
  if (mc->molecule_size > mc->wireframe_threshold)
    MI_IS_WIREFRAME(mi) = 1;

  if (MI_IS_WIREFRAME(mi))
    do_bonds = 1, do_shells = 0;

  if (!do_bonds)
    do_shells = 0;

  if (! (do_bonds || do_atoms || do_labels))
    {
      /* Make sure *something* shows up! */
      MI_IS_WIREFRAME(mi) = 1;
      do_bonds = 1;
    }

  build_molecule (mi, False);
  glEndList();

  if (do_shells)
    {
      glNewList (mc->shell_dlist, GL_COMPILE);
      ensure_bounding_box_visible (mi);

      do_labels = 0;
      do_atoms  = 1;
      do_bonds  = 0;

      build_molecule (mi, True);

      glEndList();
      do_bonds  = orig_do_bonds;
      do_atoms  = orig_do_atoms;
      do_labels = orig_do_labels;
    }
}


void
draw_molecule (ModeInfo *mi)
{
  static time_t last = 0;
  time_t now = time ((time_t *) 0);
  GLfloat speed = 4.0;  /* speed at which the zoom out/in happens */

  molecule_configuration *mc = &mcs[MI_SCREEN(mi)];
  Display *dpy = MI_DISPLAY(mi);
  Window window = MI_WINDOW(mi);

  if (!mc->glx_context)
    return;

  if (last == 0)
    {
      pick_new_molecule (mi, last);
      last = now;
    }
  else if (mc->mode == 0)
    {
      static int tick = 0;
      if (tick++ > 10)
        {
          time_t now = time((time_t *) 0);
          if (last == 0) last = now;
          tick = 0;

          if (!mc->button_down_p &&
              mc->nmolecules > 1 &&
              last + timeout <= now)
            {
              /* randomize molecules every -timeout seconds */
              mc->mode = 1;    /* go out */
              mc->mode_tick = 10 * speed;
              last = now;
            }
        }
    }
  else if (mc->mode == 1)   /* out */
    {
      if (--mc->mode_tick <= 0)
        {
          mc->mode_tick = 10 * speed;
          mc->mode = 2;  /* go in */
          pick_new_molecule (mi, last);
          last = now;
        }
    }
  else if (mc->mode == 2)   /* in */
    {
      if (--mc->mode_tick <= 0)
        mc->mode = 0;  /* normal */
    }
  else
    abort();

  glPushMatrix ();
  glScalef(1.1, 1.1, 1.1);

  {
    double x, y, z;
    get_position (mc->rot, &x, &y, &z, !mc->button_down_p);
    glTranslatef((x - 0.5) * 9,
                 (y - 0.5) * 9,
                 (z - 0.5) * 9);

    gltrackball_rotate (mc->trackball);

    get_rotation (mc->rot, &x, &y, &z, !mc->button_down_p);
    glRotatef (x * 360, 1.0, 0.0, 0.0);
    glRotatef (y * 360, 0.0, 1.0, 0.0);
    glRotatef (z * 360, 0.0, 0.0, 1.0);
  }

  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  if (mc->mode != 0)
    {
      GLfloat s = (mc->mode == 1
                   ? mc->mode_tick / (10 * speed)
                   : ((10 * speed) - mc->mode_tick + 1) / (10 * speed));
      glScalef (s, s, s);
    }

  glPushMatrix();
  glCallList (mc->molecule_dlist);

  if (mc->mode == 0)
    {
      molecule *m = &mc->molecules[mc->which];

      draw_labels (mi);

      /* This can't go in the display list, or the characters are spaced
         wrongly when the window is resized. */
      if (do_titles && m->label && *m->label)
        {
          set_atom_color (mi, 0, True, 1);
          print_gl_string (mi->dpy, mc->xfont2, mc->font2_dlist,
                           mi->xgwa.width, mi->xgwa.height,
                           10, mi->xgwa.height - 10,
                           m->label);
        }
    }
  glPopMatrix();

  if (do_shells)
    {
      glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glPushMatrix();
      glCallList (mc->shell_dlist);
      glPopMatrix();
      glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

      glDepthFunc (GL_EQUAL);
      glEnable (GL_BLEND);
      glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glPushMatrix();
      glCallList (mc->shell_dlist);
      glPopMatrix();
      glDepthFunc (GL_LESS);
      glDisable (GL_BLEND);
    }

  glPopMatrix ();

  mi->polygon_count = mc->polygon_count;

  if (mi->fps_p) do_fps (mi);
  glFinish();

  glXSwapBuffers(dpy, window);
}

#endif /* USE_GL */