/*
  ***** BEGIN LICENSE BLOCK *****

  Copyright (C) 2001-2021 Olof Hagsand

  This file is part of CLIgen.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 2 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****


  CLIgen variable vectors - cvec
*/

#include "cligen_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>

#include "cligen_buf.h"
#include "cligen_cv.h"
#include "cligen_cvec.h"
#include "cligen_parsetree.h"
#include "cligen_object.h"
#include "cligen_io.h"
#include "cligen_match.h"
#include "cligen_getline.h"

#include "cligen_cv_internal.h"
#include "cligen_cvec_internal.h"

/*! A malloc version that aligns on 4 bytes. To avoid warning from valgrind */
#define align4(s) (((s)/4)*4 + 4)

/*! A strdup version that aligns on 4 bytes. To avoid warning from valgrind */
static inline char * strdup4(char *str)
{
    char *dup;
    int len;

    len = align4(strlen(str)+1);
    if ((dup = malloc(len)) == NULL)
	return NULL;
    strcpy(dup, str);
    return dup;
}

/*
 * cv_exclude_keys
 * set if you want to backward compliant: dont include keys in cgv vec to callback
 * that is, regular 'keys' and keys like: '<string keyword=foo>'
 */
static int excludekeys = 0;

/*! Create and initialize a new cligen variable vector (cvec)
 *
 * Each individual cv initialized with CGV_ERR and no value.
 * Returned cvec needs to be freed with cvec_free().
 *
 * @param[in] len    Number of cv elements. Can be zero and elements added incrementally.
 * @retval    NULL   errno set
 * @retval    cvv    allocated cligen var vector
 * @see cvec_init
 */
cvec *
cvec_new(int len)
{
    cvec *cvv;

    if ((cvv = malloc(sizeof(*cvv))) == NULL)
	return NULL;
    memset(cvv, 0, sizeof(*cvv));
    if (cvec_init(cvv, len) < 0){
	free(cvv);
	return NULL;
    }
    return cvv;
}

/*! Create a new vector, initialize the first element to the contents of 'var'
 *
 * @param[in] var      cg_var to clone and add to vector
 * @retval    cvec     allocated cvec
 */
cvec *
cvec_from_var(cg_var *cv)
{
    cvec   *newvec = NULL;
    cg_var *tail = NULL;

    if (cv && (newvec = cvec_new(0))) {
        if ((tail = cvec_append_var(newvec, cv)) == NULL) {
            cvec_free(newvec);
            newvec = NULL;
        }
    }
    return newvec;
}

/*! Free a cligen  variable vector (cvec)
 *
 * Reset and free a cligen vector as previously created by cvec_new(). this includes
 * freeing all cv:s that the cvec consists of.
 * @param[in]  cvv   Cligen variable vector
 * @see cvec_new
 */
int
cvec_free(cvec *cvv)
{
    if (cvv) {
	cvec_reset(cvv);
	free(cvv);
    }
    return 0;
}

/*! Initialize a cligen variable vector (cvec) with 'len' numbers of variables.
 *
 * Each individual cv initialized with CGV_ERR and no value.
 *
 * @param[in] cvv  Cligen variable vector
 * @param[in] len  Number of cv elements. Can be zero and elements added incrementally.
 * @see cvec_new
 */
int
cvec_init(cvec *cvv,
	  int   len)
{
    cvv->vr_len = len;
    if (len && (cvv->vr_vec = calloc(cvv->vr_len, sizeof(cg_var))) == NULL)
	return -1;
    return 0;
}

/*! Reset cligen variable vector resetting it to an initial state as returned by cvec_new
 *
 * @param[in]  cvv   Cligen variable vector
 * @see also cvec_free. But this function does not actually free the cvec.
 */
int
cvec_reset(cvec *cvv)
{
    cg_var *cv = NULL;

    if (cvv == NULL)
	return 0;
    while ((cv = cvec_each(cvv, cv)) != NULL)
	cv_reset(cv);
    if (cvv->vr_vec)
	free(cvv->vr_vec);
    if (cvv->vr_name)
	free(cvv->vr_name);
    memset(cvv, 0, sizeof(*cvv));
    return 0;
}

/*! Given a cv in a cligen variable vector (cvec) return the next cv.
 *
 * @param[in]  cvv    The cligen variable vector
 * @param[in]  cv0    Return element after this, or first element if this is NULL
 * Given an element (cv0) in a cligen variable vector (cvec) return the next element.
 * @retval cv  Next element
 */
cg_var *
cvec_next(cvec   *cvv,
	  cg_var *cv0)
{
    cg_var *cv = NULL;
    int i;

    if (cvv == NULL)
	return NULL;
    if (cv0 == NULL)
	cv = cvv->vr_vec;
    else {
	i = cv0 - cvv->vr_vec;
	if (i < cvv->vr_len-1)
	    cv = cv0 + 1;
    }
    return cv;
}

/*! Append a new cligen variable (cv) to cligen variable vector (cvec) and return it.
 *
 * @param[in] cvv   Cligen variable vector
 * @param[in] type  Append a new cv to the vector with this type
 * @retval    NULL  Error
 * @retval    cv    The new cligen variable
 * @see also cv_new, but this is allocated contiguosly as a part of a cvec.
 */
cg_var *
cvec_add(cvec        *cvv,
	 enum cv_type type)
{
    int     len;
    cg_var *cv;

    if (cvv == NULL){
	errno = EINVAL;
	return NULL;
    }
    len = cvv->vr_len + 1;

    if ((cvv->vr_vec = realloc(cvv->vr_vec, len*sizeof(cg_var))) == NULL)
	return NULL;
    cvv->vr_len = len;
    cv = cvec_i(cvv, len-1);
    memset(cv, 0, sizeof(*cv));
    cv->var_type = type;
    return cv;
}

/*! Append a new var that is a clone of data in 'cv' to the vector, return it
 * @param[in] cvv  Cligen variable vector
 * @param[in] cv   Append this cligen variable to vector. Note that it is copied.
 * @retval    NULL Error
 * @retval    tail Return the new last tail variable (copy of cv)
 */
cg_var *
cvec_append_var(cvec   *cvv,
		cg_var *cv)
{
    cg_var *tail = NULL;

    if (cvv && cv && (tail = cvec_add(cvv, cv_type_get(cv)))) {
        if (cv_cp(tail, cv) < 0) {
            cvec_del(cvv, tail);
            tail = NULL;
        }
    }
    return tail;
}

/*! Delete a cv variable from a cvec. Note: cv is not reset & cv may be stale!
 *
 * @param[in]  cvv   Cligen variable vector
 * @param[in]  del   variable to delete
 *
 * @note This is a dangerous command since the cv it deletes (such as created by
 * cvec_add) may have been modified with realloc (eg cvec_add/delete) and
 * therefore can not be used as a reference.  Safer methods are to use
 * cvec_find/cvec_i to find a cv and then to immediately remove it.
 */
int
cvec_del(cvec   *cvv,
	 cg_var *del)
{
    int i;
    cg_var *cv;

    if (cvec_len(cvv) == 0)
	return 0;

    i = 0;
    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL) {
	if (cv == del)
	    break;
	i++;
    }
    if (i >= cvec_len(cvv)) /* Not found !?! */
	return cvec_len(cvv);

    if (i != cvec_len(cvv)-1) /* If not last entry, move the remaining cv's */
	memmove(&cvv->vr_vec[i], &cvv->vr_vec[i+1],
		(cvv->vr_len-i-1) * sizeof(cvv->vr_vec[0]));

    cvv->vr_len--;
    cvv->vr_vec = realloc(cvv->vr_vec, cvv->vr_len*sizeof(cvv->vr_vec[0])); /* Shrink should not fail? */

    return cvec_len(cvv);
}

/*! Delete a cv variable from a cvec using index i
 *
 * @param[in]  cvv   Cligen variable vector
 * @param[in]  del   variable to delete
 *
 * @note This is a dangerous command since the cv it deletes (such as created by
 * cvec_add) may have been modified with realloc (eg cvec_add/delete) and
 * therefore can not be used as a reference.  Safer methods are to use
 * cvec_find/cvec_i to find a cv and then to immediately remove it.
 * @note does not deallocate the cv, you may need to do it with cv_reset
 */
int
cvec_del_i(cvec *cvv,
	   int   i)
{
    if (cvec_len(cvv) == 0 || cvec_len(cvv) < i)
	return 0;
    
    if (i != cvec_len(cvv)-1) /* If not last entry, move the remaining cv's */
	memmove(&cvv->vr_vec[i], &cvv->vr_vec[i+1],
		(cvv->vr_len-i-1) * sizeof(cvv->vr_vec[0]));

    cvv->vr_len--;

    return cvec_len(cvv);
}

/*! Return allocated length of a cvec.
 * @param[in]  cvv   Cligen variable vector
 */
int
cvec_len(cvec *cvv)
{
    if (cvv == NULL)
	return 0;
    return cvv->vr_len;
}

/*! Return i:th element of cligen variable vector cvec.
 * @param[in]  cvv   Cligen variable vector
 * @param[in]  i     Order of element to get
 */
cg_var *
cvec_i(cvec *cvv,
       int   i)
{
	if (!cvv) {
		return NULL;
	}
    if (i < cvv->vr_len)
	return &cvv->vr_vec[i];
    return NULL;
}

/*! Return string value of i:th element of cligen variable vector. Helper function.
 * @param[in]  cvv   Cligen variable vector
 * @param[in]  i     Order of element to get
 * @retval     str   String value of i:th element
 * @retval     NULL  Element does not exist
 */
char *
cvec_i_str(cvec *cvv,
	   int   i)
{
    cg_var *cv;
    
    if ((cv = cvec_i(cvv, i)) == NULL)
	return NULL;
    return cv_string_get(cv);
}

/*! Iterate through all cligen variables in a cvec list
 *
 * @param[in] cvv       Cligen variable vector
 * @param[in] prev	Last cgv (or NULL)
 * @retval cv           Next variable structure.
 * @retval NULL         When end of list reached.
 * @code
 *    cg_var *cv = NULL;
 *    while ((cv = cvec_each(cvv, cv)) != NULL)
 *	     ...
 * @endcode
 * @see cvec_each1 Skip first
 */
cg_var *
cvec_each(cvec   *cvv,
	  cg_var *prev)
{
    if (cvv == NULL)
	return NULL;
    if (prev == NULL){   /* Initialization */
	if (cvv->vr_len > 0)
	    return &cvv->vr_vec[0];
	else
	    return NULL;
    }
    return cvec_next(cvv, prev);
}

/*! Iterate through all except first cligen variables in a cvec list
 *
 * @param[in] cvv   Cligen variable vector
 * @param[in] prev  Last cgv (or NULL)
 * @retval cv       Next variable structure.
 * @retval NULL     When end of list reached.
 * Common in many cvecs where [0] is the command-line and all
 * others are arguments.
 * @see cvec_each  For all elements, dont skip first
 */
cg_var *
cvec_each1(cvec   *cvv,
	   cg_var *prev)
{
    if (cvv == NULL)
	return NULL;
    if (prev == NULL){   /* Initialization */
	if (cvv->vr_len > 1)
	    return &cvv->vr_vec[1];
	else
	    return NULL;
    }
    return cvec_next(cvv, prev);
}

/*! Create a new cvec by copying from an original
 *
 * @param[in]   old   The cvec to copy from
 * @retval      new   The cvec copy. Free this with cvec_free
 * @retval      NULL  Error
 * The new cvec needs to be freed by cvec_free().
 * One can make a cvec_cp() as well but it is a little trickier to match vr_vec.
 */
cvec *
cvec_dup(cvec *old)
{
    cvec   *new;
    cg_var *cv0 = NULL;
    cg_var *cv1;
    int     i;

    if (old == NULL)
	return NULL;
    if ((new = cvec_new(old->vr_len)) == NULL)
	return NULL;
    if (old->vr_name)
	if ((new->vr_name = strdup4(old->vr_name)) == NULL)
	    return NULL;
    i = 0;
    while ((cv0 = cvec_each(old, cv0)) != NULL) {
	cv1 = cvec_i(new, i++);
	cv_cp(cv1, cv0);
    }
    return new;
}

/*! Create a cv list with a single string element.
 *
 * @param[in]  cmd  Text string
 * @retval     NULL Error
 * @retval     cvv  Cligen variable list
 * Help function when creating cvec to cligen callbacks.
 */
cvec *
cvec_start(char *cmd)
{
    cvec *cvec;
    cg_var    *cv;

    if ((cvec = cvec_new(1)) == NULL)
	return NULL;
    cv = cvec_i(cvec, 0);
    cv->var_type = CGV_REST;
    cv_name_set(cv, "cmd"); /* the whole command string */
    cv_string_set(cv, cmd); /* the whole command string */
    return cvec;
}

/*! Pretty print cligen variable list to a file
 * @param[in]  f    File to print to
 * @param[in]  cvv  Cligen variable vector to print
 * @see cvec2cbuf
 */
int
cvec_print(FILE *f,
	   cvec *cvv)
{
    cg_var *cv = NULL;
    char   *name;
    int     i = 0;

    if ((name = cvec_name_get(cvv)) != NULL)
	fprintf(f, "%s:\n", name);
    while ((cv = cvec_each(cvv, cv)) != NULL) {
	name = cv_name_get(cv);
	if (name)
	    fprintf(f, "%d : %s = ", i++, name);
	else
	    fprintf(f, "%d : ", i++);
	cv_print(f, cv);
	fprintf(f, "\n");
    }
    return 0;
}

/*! Pretty print cligen variable list to a cligen buffer
 * @param[out] cb   Cligen buffer (should already be initialized w cbuf_new)
 * @param[in]  cvv  Cligen variable vector to print
 * @see cvec_print
 */
int
cvec2cbuf(cbuf *cb,
	  cvec *cvv)
{
    cg_var *cv = NULL;
    int     i = 0;
    char   *s;

    while ((cv = cvec_each(cvv, cv)) != NULL) {
	if ((s = cv2str_dup(cv)) == NULL)
	    return -1;
	cprintf(cb, "%d : %s = %s\n", i++, cv_name_get(cv), s);
	free(s);
    }
    return 0;
}

/*! Return first cv in a cvec matching a name
 *
 * Given an CLIgen variable vector cvec, and the name of a variable, return the
 * first matching entry.
 * @param[in]  cvv   Cligen variable vector
 * @param[in]  name  Name to match (can be NULL)
 * @retval     cv    Element matching name. NULL
 * @retval     NULL  Not found
 * @see cvec_find_keyword
 */
cg_var *
cvec_find(cvec *cvv,
	  char *name)
{
    cg_var *cv = NULL;

    while ((cv = cvec_each(cvv, cv)) != NULL){
	if (cv->var_name){
	    if (name != NULL && strcmp(cv->var_name, name) == 0)
		return cv;
	}
	else if (name == NULL)
	    return cv;
    }
    return NULL;
}

/*! Return first keyword cv in a cvec matching a name
 * @param[in]  cvv   Cligen variable vector
 * @param[in]  name  Name to match
 * @retval     cv    Element matching name. NULL
 * @retval     NULL  Not found
 * @see cvec_find
 */
cg_var *
cvec_find_keyword(cvec *cvv,
		  char *name)
{
    cg_var *cv = NULL;

    while ((cv = cvec_each(cvv, cv)) != NULL)
	if (cv->var_name && strcmp(cv->var_name, name) == 0 && cv->var_const)
	    return cv;
    return NULL;
}

/*! Return first non-keyword cv in a cvec matching a name
 * @param[in]  cvv   Cligen variable vector
 * @param[in]  name  Name to match
 * @retval     cv    Element matching name. NULL
 * @retval     NULL  Not found
 * @see cvec_find
 */
cg_var *
cvec_find_var(cvec *cvv,
	      char *name)
{
    cg_var *cv = NULL;

    while ((cv = cvec_each(cvv, cv)) != NULL)
	if (cv->var_name && strcmp(cv->var_name, name) == 0 && !cv->var_const)
	    return cv;
    return NULL;
}

/*! Typed version of cvec_find that returns the string value.
 *

 * @param[in]  cvv   Cligen variable vector
 * @param[in]  name  Name to match
 * @retval     cv    Element matching name. NULL
 * @retval     NULL  Not found
 * @note Does not see the difference between not finding the cv, and finding one
 *           with wrong type - in both cases NULL is returned.
 * @note The returned string must be copied since it points directly into the cv.
 * @see cvec_find
 */
char *
cvec_find_str(cvec *cvv,
	      char *name)
{
    cg_var *cv;

    if ((cv = cvec_find(cvv, name)) != NULL && cv_isstring(cv->var_type))
	return cv_string_get(cv);
    return NULL;
}

/*! Get name of cligen variable vector
 * @param[in]  cvv  Cligen variable vector
 * @retval str  The name of the cvec as a string, can be NULL, no copy
 * @retval      name Name of variable vector
 */
char *
cvec_name_get(cvec *cvv)
{
    return cvv->vr_name;
}

/*! Allocate and set name of cligen variable vector, including NULL
 * @param[in]  cvv    A cligen variable vector
 * @param[in]  name   A string that is copied and used as a cvec name, or NULL
 * @retval     str    The name of the cvec.
 * The existing name, if any, is freed
 */
char *
cvec_name_set(cvec *cvv,
	      char *name)
{
    char *s1 = NULL;

    /* Duplicate name. Must be done before a free, in case name is part of the original */
    if (name){
	if ((s1 = strdup4(name)) == NULL)
	    return NULL; /* error in errno */
    }
    if (cvv->vr_name != NULL)
	free(cvv->vr_name);
    cvv->vr_name = s1;
    return s1;
}


/*! Changes cvec find function behaviour, exclude keywords or include them.
 * @param[in] status
 */
int
cv_exclude_keys(int status)
{
    excludekeys = status;
    return 0;
}
/*! Changes cvec find function behaviour, exclude keywords or include them.
 * @param[in] status
 */
int
cv_exclude_keys_get(void)
{
    return excludekeys;
}

/*! Return the alloced memory of a CLIgen variable vector
 */
size_t
cvec_size(cvec *cvv)
{
    size_t  sz = 0;
    cg_var *cv = NULL;

    sz += sizeof(struct cvec);
    if (cvv->vr_name)
	sz += strlen(cvv->vr_name)+1;
    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL)
	sz += cv_size(cv);
    return sz;
}
