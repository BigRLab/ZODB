/*****************************************************************************

  Copyright (c) 2001, 2002 Zope Corporation and Contributors.
  All Rights Reserved.

  This software is subject to the provisions of the Zope Public License,
  Version 2.0 (ZPL).  A copy of the ZPL should accompany this distribution.
  THIS SOFTWARE IS PROVIDED "AS IS" AND ANY AND ALL EXPRESS OR IMPLIED
  WARRANTIES ARE DISCLAIMED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF TITLE, MERCHANTABILITY, AGAINST INFRINGEMENT, AND FITNESS
  FOR A PARTICULAR PURPOSE

 ****************************************************************************/

static char cPickleCache_doc_string[] =
"Defines the PickleCache used by ZODB Connection objects.\n"
"\n"
"$Id: cPickleCache.c,v 1.43 2002/04/01 23:36:34 jeremy Exp $\n";

#define ASSIGN(V,E) {PyObject *__e; __e=(E); Py_XDECREF(V); (V)=__e;}
#define UNLESS(E) if(!(E))
#define UNLESS_ASSIGN(V,E) ASSIGN(V,E) UNLESS(V)
#define OBJECT(O) ((PyObject*)O)

#define DONT_USE_CPERSISTENCECAPI
#include "cPersistence.h"
#include <time.h>
#include <stddef.h>

#undef Py_FindMethod


static PyObject *py__p_oid, *py_reload, *py__p_jar, *py__p_changed;

/* define this for extra debugging checks, and lousy performance */
#define MUCH_RING_CHECKING 1

/* Do we want 'engine noise'.... abstract debugging output useful for
   visualizing cache behavior */
#if 0
#define ENGINE_NOISE(A) printf(A)
#else
#define ENGINE_NOISE(A) ((void)A)
#endif

/* the layout of this struct is the same as the start of ccobject_head in cPersistence.c */
typedef struct {
    CACHE_HEAD
    int klass_count;
    PyObject *data;
    PyObject *jar;
    PyObject *setklassstate;
    int cache_size;
    int ring_lock;
    int cache_drain_resistance;
} ccobject;

staticforward PyTypeObject Cctype;


staticforward int present_in_ring(ccobject *self,CPersistentRing *target);
staticforward int check_ring(ccobject *self,const char *context);
staticforward int cc_ass_sub(ccobject *self, PyObject *key, PyObject *v);

/* ---------------------------------------------------------------- */

static PyObject *object_from_oid(ccobject *self,PyObject *key)
/* somewhat of a replacement for PyDict_GetItem(self->data....
   however this returns a *new* reference */
{
    PyObject *v = PyDict_GetItem(self->data, key);
    if(!v) return NULL;

    Py_INCREF(v);

    return v;
}

static cPersistentObject *
object_from_ring(ccobject *self, CPersistentRing *here, const char *context)
{
    /* Given a position in the LRU ring, return a borrowed
    reference to the object at that point in the ring. The caller is
    responsible for ensuring that this ring position really does
    correspond to a persistent object, although the debugging
    version will double-check this. */

    PyObject *object;

    object = (PyObject *)(((char *)here) - offsetof(cPersistentObject, ring));

#ifdef MUCH_RING_CHECKING
    if (!PyExtensionInstance_Check(object)) {
        PyErr_Format(PyExc_RuntimeError,
	     "Unexpectedly encountered non-ExtensionClass object in %s",
		     context);
        return NULL;
    }
    if (!(((PyExtensionClass*)(object->ob_type))->class_flags & PERSISTENT_TYPE_FLAG)) {
        PyErr_Format(PyExc_RuntimeError,
	     "Unexpectedly encountered non-persistent object in %s", context);
        return NULL;
    }
    if (((cPersistentObject*)object)->jar != self->jar) {
        PyErr_Format(PyExc_RuntimeError,
	     "Unexpectedly encountered object from a different jar in %s",
		     context);
        return NULL;
    }
    if (((cPersistentObject *)object)->cache != (PerCache *)self) {
        PyErr_Format(PyExc_RuntimeError,
		     "Unexpectedly encountered broken ring in %s", context);
        return NULL;
    }
#endif
    return (cPersistentObject *)object;
}

static int
scan_gc_items(ccobject *self,int target)
{
    cPersistentObject *object;
    int error;
    CPersistentRing placeholder;
    CPersistentRing *here = self->ring_home.next;

#ifdef MUCH_RING_CHECKING
    int safety_counter = self->cache_size*10;
    if(safety_counter<10000) safety_counter = 10000;
#endif

    while(1)
    {
        if(check_ring(self,"mid-gc")) return -1;

#ifdef MUCH_RING_CHECKING
        if(!safety_counter--)
        {
            /* This loop has been running for a very long time.
               It is possible that someone loaded a very large number of objects,
               and now wants us to blow them all away. However it may
               also indicate a logic error. If the loop has been running this
               long then you really have to doubt it will ever terminate.
               In the MUCH_RING_CHECKING build we prefer to raise an exception
               here */
            PyErr_SetString(PyExc_RuntimeError,"scan_gc_items safety counter exceeded");
            return -1;
        }

        if(!present_in_ring(self,here))
        {
            /* Our current working position is no longer in the ring. Thats bad. */
            PyErr_SetString(PyExc_RuntimeError,"working position fell out the ring, in scan_gc_items");
            return -1;
        }
#endif

        if(here==&self->ring_home)
        {
            /* back to the home position. stop looking */
            return 0;
        }

        /* At this point we know that the ring only contains nodes from
        persistent objects, plus our own home node. We can safely
        assume this is a persistent object now we know it is not the home */
        object = object_from_ring(self,here,"scan_gc_items");
        if(!object) return -1;

        if(self->non_ghost_count<=target)
        {
            /* we are small enough */
            return 0;
        }
        else if(object->state==cPersistent_UPTODATE_STATE)
        {
            /* deactivate it. This is the main memory saver. */

            ENGINE_NOISE("G");

            /* add a placeholder */
            placeholder.next = here->next;
            placeholder.prev = here;
            here->next->prev = &placeholder;
            here->next       = &placeholder;

            error = PyObject_SetAttr((PyObject *)object,py__p_changed,Py_None);

            /* unlink the placeholder */
            placeholder.next->prev=placeholder.prev;
            placeholder.prev->next=placeholder.next;

            here = placeholder.next;

            if(error)
                return -1; /* problem */
        }
        else
        {
            ENGINE_NOISE(".");

            here = here->next;
        }
    }
}


static PyObject *
lockgc(ccobject *self,int target_size)
{
    if(self->ring_lock)
    {
        Py_INCREF(Py_None);
        return Py_None;
    }

    if(check_ring(self,"pre-gc")) return NULL;
    ENGINE_NOISE("<");
    self->ring_lock = 1;
    if(scan_gc_items(self,target_size))
    {
        self->ring_lock = 0;
        return NULL;
    }
    self->ring_lock = 0;
    ENGINE_NOISE(">\n");
    if(check_ring(self,"post-gc")) return NULL;

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
cc_incrgc(ccobject *self, PyObject *args)
{
    int n=1;

    int starting_size = self->non_ghost_count;

    int target_size = self->cache_size;

    if(self->cache_drain_resistance>=1)
    {
        /* This cache will gradually drain down to a small size. Check
           a (small) number of objects proportional to the current size */

        int target_size_2 = starting_size - 1 - starting_size/self->cache_drain_resistance;
        if(target_size_2<target_size)
            target_size = target_size_2;
    }

    UNLESS (PyArg_ParseTuple(args, "|i",&n)) return NULL;

    return lockgc(self,target_size);
}

static PyObject *
cc_full_sweep(ccobject *self, PyObject *args)
{
  int dt=0;
  UNLESS(PyArg_ParseTuple(args, "|i", &dt)) return NULL;
  return lockgc(self,0);
}

static PyObject *
cc_reallyfull_sweep(ccobject *self, PyObject *args)
{
  int dt=0;
  UNLESS(PyArg_ParseTuple(args, "|i", &dt)) return NULL;
  return lockgc(self,0);
}

static void
_invalidate(ccobject *self, PyObject *key)
{
    PyObject *v=object_from_oid(self, key);

    if(!v)
    {
        /* shouldnt this be an error? for now Ill follow Jims lead */
        PyErr_Clear();
    }
    else
    {
        if (PyExtensionClass_Check(v))
        {
            if(v->ob_refcnt <= 1)
            {
                self->klass_count--;
                if (PyDict_DelItem(self->data, key) < 0)
                  PyErr_Clear();
            }
            else
            {
                v=PyObject_CallFunction(self->setklassstate,
                                        "O", v);
                if (v) Py_DECREF(v);
                else PyErr_Clear();
            }
        }
        else
        {
            if(PyObject_DelAttr(v,py__p_changed) < 0)
                PyErr_Clear();
        }
        Py_DECREF(v);
    }
}

static PyObject *
cc_invalidate(ccobject *self, PyObject *args)
{
  PyObject *inv, *key, *v;
  int i;
  
  if (PyArg_ParseTuple(args, "O!", &PyDict_Type, &inv)) {
    for (i=0; PyDict_Next(inv, &i, &key, &v); ) 
      if (key==Py_None)
	{ /* Eek some nitwit invalidated everything! */
	  for (i=0; PyDict_Next(self->data, &i, &key, &v); )
	    _invalidate(self, key);
	  break;
	}
      else
	_invalidate(self, key);
    PyDict_Clear(inv);
  }
  else {
    PyErr_Clear();
    UNLESS (PyArg_ParseTuple(args, "O", &inv)) return NULL;
    if (PyString_Check(inv))
      _invalidate(self, inv);
    else if (inv==Py_None)	/* All */
      for (i=0; PyDict_Next(self->data, &i, &key, &v); )
	_invalidate(self, key);
    else {
      int l;

      PyErr_Clear();
      if ((l=PyObject_Length(inv)) < 0) return NULL;
      for(i=l; --i >= 0; )
	{
	  UNLESS (key=PySequence_GetItem(inv, i)) return NULL;
	  _invalidate(self, key);
	  Py_DECREF(key);
	}
      PySequence_DelSlice(inv, 0, l);
    }
  }

  Py_INCREF(Py_None);
  return Py_None;
}
  
  
static PyObject *
cc_get(ccobject *self, PyObject *args)
{
  PyObject *r, *key, *d=0;

  UNLESS (PyArg_ParseTuple(args,"O|O", &key, &d)) return NULL;

  UNLESS (r=(PyObject *)object_from_oid(self, key))
    {
      if (d) 
	{
	  PyErr_Clear();
	  r=d;
          Py_INCREF(r);
	}
      else
	{
	  PyErr_SetObject(PyExc_KeyError, key);
	  return NULL;
	}
    }

  return r;
}

static PyObject *
cc_klass_items(ccobject *self, PyObject *args)
{
    PyObject *l,*k,*v;
    int p = 0;

    if(!PyArg_ParseTuple(args,"")) return NULL;

    l = PyList_New(0);
    if(!l) return NULL;

    while(PyDict_Next(self->data, &p, &k, &v))
    {
        if(PyExtensionClass_Check(v))
        {
            v=PyObject_CallMethod(l,"append","((OO))",k,v);
            if(!v)
            {
                Py_DECREF(l);
                return NULL;
            }
        }
    }

    return l;
}

static PyObject *
cc_lru_items(ccobject *self, PyObject *args)
{
    PyObject *l;
    CPersistentRing *here;

    if(!PyArg_ParseTuple(args,"")) return NULL;

    if(self->ring_lock)
    {
        PyErr_SetString(PyExc_ValueError,".lru_items() is unavailable during garbage collection");
        return NULL;
    }

    if(check_ring(self,"pre-cc_items")) return NULL;

    l = PyList_New(0);
    if(!l) return NULL;

    here = self->ring_home.next;
    while(here!=&self->ring_home)
    {
        cPersistentObject *object = object_from_ring(self,here,"cc_items");
        PyObject *v;
        if(!object)
        {
            Py_DECREF(l);
            return NULL;
        }
        v=PyObject_CallMethod(l,"append","((OO))",object->oid,object);
        if(!v)
        {
            Py_DECREF(l);
            return NULL;
        }
        Py_DECREF(v);
        here = here->next;
    }

    return l;
}

static PyObject *
cc_oid_unreferenced(ccobject *self, PyObject *args)
{
    PyObject *oid,*v;
    if(!PyArg_ParseTuple(args,"O",&oid)) return NULL;

    v = PyDict_GetItem(self->data, oid);
    if(!v) return NULL;

    if(v->ob_refcnt)
    {
        PyErr_Format(PyExc_ValueError,"object has reference count of %d, should be zero",v->ob_refcnt);
        return NULL;
    }

    /* Need to be very hairy here because a dictionary is about
       to decref an already deleted object */

#ifdef Py_TRACE_REFS
#error "this code path has not been tested - Toby Dickenson"
    _Py_NewReference(v);
    /* it may be a problem that v->ob_type is still NULL? */
#else
    Py_INCREF(v);
#endif

    if(v->ob_refcnt!=1)
    {
        PyErr_SetString(PyExc_ValueError,"refcount is not 1 after resurrection");
        return NULL;
    }

    /* return the stolen reference */
    Py_INCREF(v);

    PyDict_DelItem(self->data, oid);

    if(v->ob_refcnt!=1)
    {
        PyErr_SetString(PyExc_ValueError,"refcount is not 1 after removal from dict");
        return NULL;
    }

    /* undo the temporary resurrection */
#ifdef Py_TRACE_REFS
    _Py_ForgetReference(v);
#else
    v->ob_refcnt=0;
#endif

    Py_INCREF(Py_None);
    return Py_None;
}


static struct PyMethodDef cc_methods[] = {
  {"_oid_unreferenced", (PyCFunction)cc_oid_unreferenced, METH_VARARGS,
   NULL
   },
  {"lru_items", (PyCFunction)cc_lru_items, METH_VARARGS,
   "List (oid, object) pairs from the lru list, as 2-tuples.\n"
   },
  {"klass_items", (PyCFunction)cc_klass_items, METH_VARARGS,
   "List (oid, object) pairs of cached persistent classes.\n"
   },
  {"full_sweep", (PyCFunction)cc_full_sweep, METH_VARARGS,
   "full_sweep([age]) -- Perform a full sweep of the cache\n\n"
   "Make a single pass through the cache, removing any objects that are no\n"
   "longer referenced, and deactivating enough objects to bring\n"
   "the cache under its size limit\n"
   "The optional 'age' parameter is ignored.\n"
   },
  {"minimize",	(PyCFunction)cc_reallyfull_sweep, METH_VARARGS,
   "minimize([age]) -- Remove as many objects as possible\n\n"
   "Make multiple passes through the cache, removing any objects that are no\n"
   "longer referenced, and deactivating enough objects to bring the"
   " cache under its size limit\n"
   "The option 'age' parameter is ignored.\n"
   },
  {"incrgc", (PyCFunction)cc_incrgc, METH_VARARGS,
   "incrgc([n]) -- Perform incremental garbage collection\n\n"
   "Some other implementations support an optional parameter 'n' which\n"
   "indicates a repetition count; this value is ignored.\n"},
  {"invalidate", (PyCFunction)cc_invalidate, METH_VARARGS,
   "invalidate(oids) -- invalidate one, many, or all ids"},
  {"get", (PyCFunction)cc_get, METH_VARARGS,
   "get(key [, default]) -- get an item, or a default"},
  {NULL,		NULL}		/* sentinel */
};

static ccobject *
newccobject(PyObject *jar, int cache_size, int cache_age)
{
  ccobject *self;
  
  UNLESS(self = PyObject_NEW(ccobject, &Cctype)) return NULL;
  self->setklassstate=self->jar=NULL;
  if((self->data=PyDict_New()))
    {
      self->jar=jar; 
      Py_INCREF(jar);
      UNLESS (self->setklassstate=PyObject_GetAttrString(jar, "setklassstate"))
	return NULL;
      self->cache_size=cache_size;
      self->non_ghost_count=0;
      self->klass_count=0;
      self->cache_drain_resistance=0;
      self->ring_lock=0;
      self->ring_home.next = &self->ring_home;
      self->ring_home.prev = &self->ring_home;
      return self;
    }
  Py_DECREF(self);
  return NULL;
}

static void
cc_dealloc(ccobject *self)
{
  Py_XDECREF(self->data);
  Py_XDECREF(self->jar);
  Py_XDECREF(self->setklassstate);
  PyMem_DEL(self);
}

static PyObject *
cc_getattr(ccobject *self, char *name)
{
  PyObject *r;

  if(check_ring(self,"getattr")) return NULL;

  if(*name=='c')
    {
      if(strcmp(name,"cache_age")==0)
	return PyInt_FromLong(0);   /* this cache does not use this value */
      if(strcmp(name,"cache_size")==0)
	return PyInt_FromLong(self->cache_size);
      if(strcmp(name,"cache_drain_resistance")==0)
	return PyInt_FromLong(self->cache_drain_resistance);
      if(strcmp(name,"cache_non_ghost_count")==0)
	return PyInt_FromLong(self->non_ghost_count);
      if(strcmp(name,"cache_klass_count")==0)
	return PyInt_FromLong(self->klass_count);
      if(strcmp(name,"cache_data")==0)
	{
	  /* now a copy of our data; the ring is too fragile */
	  return PyDict_Copy(self->data);
	}
    }
  if((*name=='h' && strcmp(name, "has_key")==0) ||
     (*name=='i' && strcmp(name, "items")==0) ||
     (*name=='k' && strcmp(name, "keys")==0)
     )
    return PyObject_GetAttrString(self->data, name);

  if((r=Py_FindMethod(cc_methods, (PyObject *)self, name)))
    return r;
  PyErr_Clear();
  return PyObject_GetAttrString(self->data, name);
}

static int
cc_setattr(ccobject *self, char *name, PyObject *value)
{
  if(value)
    {
      int v;

      if(strcmp(name,"cache_age")==0)
	{
	  /* this cache doesnt use the age */
	  return 0;
	}

      if(strcmp(name,"cache_size")==0)
	{
	  UNLESS(PyArg_Parse(value,"i",&v)) return -1;
	  self->cache_size=v;
	  return 0;
	}

      if(strcmp(name,"cache_drain_resistance")==0)
	{
	  UNLESS(PyArg_Parse(value,"i",&v)) return -1;
	  self->cache_drain_resistance=v;
	  return 0;
	}
    }
  PyErr_SetString(PyExc_AttributeError, name);
  return -1;
}

static int
cc_length(ccobject *self)
{
  return PyObject_Length(self->data);
}
  
static PyObject *
cc_subscript(ccobject *self, PyObject *key)
{
  PyObject *r;

  if(check_ring(self,"__getitem__")) return NULL;

  UNLESS (r=(PyObject *)object_from_oid(self, key))
  {
    PyErr_SetObject(PyExc_KeyError, key);
    return NULL;
  }

  return r;
}

static int
cc_ass_sub(ccobject *self, PyObject *key, PyObject *v)
{
    int result;
    if(v)
    {
        if( ( PyExtensionInstance_Check(v) &&
              (((PyExtensionClass*)(v->ob_type))->class_flags & PERSISTENT_TYPE_FLAG) &&
              (v->ob_type->tp_basicsize >= sizeof(cPersistentObject))
            )
            ||
            PyExtensionClass_Check(v)
          )
        {
            PyObject *oid = PyObject_GetAttr(v,py__p_oid);
            PyObject *object_again;
            if(!oid)
            {
                return -1;
            }
            if(PyObject_Cmp(key,oid,&result))
            {
                Py_DECREF(oid);
                return -1;
            }
            Py_DECREF(oid);
            if(result)
            {
                PyErr_SetString(PyExc_ValueError,"key must be the same as the object's oid attribute");
                return -1;
            }
            object_again = object_from_oid(self, key);
            if(object_again)
            {
                if(object_again!=v)
                {
                    Py_DECREF(object_again);
                    PyErr_SetString(PyExc_ValueError,"Can not re-register object under a different oid");
                    return -1;
                }
                else
                {
                    /* re-register under the same oid - no work needed */
                    Py_DECREF(object_again);
                    return 0;
                }
            }
            if(PyExtensionClass_Check(v))
            {
                if(PyDict_SetItem(self->data, key, v)) return -1;
                self->klass_count++;
                return 0;
            }
            else
            {
                if(((cPersistentObject*)v)->cache) {
                    if(((cPersistentObject*)v)->cache != (PerCache *)self) {
                        /* This object is already in a different cache. */
                        PyErr_SetString(PyExc_ValueError, 
				"Cache values may only be in one cache.");
                        return -1;
                    } 
		    /* else:
		       
		       This object is already one of ours, which
		       is ok.  It would be very strange if someone
		       was trying to register the same object under a
		       different key. 
		    */
                }

                if(check_ring(self,"pre-setitem")) return -1;
                if(PyDict_SetItem(self->data, key, v)) return -1;

                Py_INCREF(self);
                ((cPersistentObject*)v)->cache = (PerCache *)self;
                if(((cPersistentObject*)v)->state>=0)
                {
                    /* insert this non-ghost object into the ring just behind the home position */
                    self->non_ghost_count++;
                    ((cPersistentObject*)v)->ring.next = &self->ring_home;
                    ((cPersistentObject*)v)->ring.prev = self->ring_home.prev;
                    self->ring_home.prev->next = &((cPersistentObject*)v)->ring;
                    self->ring_home.prev = &((cPersistentObject*)v)->ring;
                }
                else
                {
                    /* steal a reference from the dictionary; ghosts have a weak reference */
                    Py_DECREF(v);
                }

                if(check_ring(self,"post-setitem")) return -1;
                return 0;
            }
        }
        else
        {
            PyErr_SetString(PyExc_ValueError, "Cache values must be persistent objects.");
            return -1;
        }
    }
    else
    {
        /* unlink this item from the ring */
        if(check_ring(self,"pre-delitem")) return -1;

        v = (PyObject *)object_from_oid(self,key);
        if(!v) return -1;

        if(PyExtensionClass_Check(v))
        {
            self->klass_count--;
        }
        else
        {
            if(((cPersistentObject*)v)->state>=0)
            {
                self->non_ghost_count--;
                ((cPersistentObject*)v)->ring.next->prev = ((cPersistentObject*)v)->ring.prev;
                ((cPersistentObject*)v)->ring.prev->next = ((cPersistentObject*)v)->ring.next;
                ((cPersistentObject*)v)->ring.prev = NULL;
                ((cPersistentObject*)v)->ring.next = NULL;
            }
            else
            {
                /* This is a ghost object, so we havent kept a reference count on it.
                For it have stayed alive this long someone else must be keeping a reference
                to it. Therefore we need to temporarily give it back a reference count
                before calling DelItem below */
                Py_INCREF(v);
            }

            Py_DECREF((PyObject *)((cPersistentObject*)v)->cache);
            ((cPersistentObject*)v)->cache = NULL;
        }

        Py_DECREF(v);

        if(PyDict_DelItem(self->data, key))
        {
            PyErr_SetString(PyExc_RuntimeError,
                           "unexpectedly couldnt remove key in cc_ass_sub");
            return -1;
        }

        if(check_ring(self,"post-delitem")) return -1;

        return 0;
    }
}

static int _check_ring(ccobject *self,const char *context)
{

    CPersistentRing *here = &(self->ring_home);
    int expected = 1+self->non_ghost_count;
    int total = 0;
    do
    {
        if(++total>(expected+10)) return 3;            /* ring too big, by a large margin */
        if(!here->next) return 4;                      /* various linking problems */
        if(!here->prev) return 5;
        if(!here->next->prev) return 7;
        if(!here->prev->next) return 8;
        if(here->prev->next!=here) return 9;
        if(here->next->prev!=here) return 10;
        if(!self->ring_lock)
        {
            /* if the ring must be locked then it only contains object other than persistent instances */
            if(here!=&self->ring_home)
            {
                cPersistentObject *object = object_from_ring(self,here,context);
                if(!object) return 12;
                if(object->state==cPersistent_GHOST_STATE)
                    return 13;
            }
        }
        here = here->next;
    }
    while(here!=&self->ring_home);

    if(self->ring_lock)
    {
        if(total<expected) return 6;       /* ring too small; too big is ok when locked */
    }
    else
    {
        if(total!=expected) return 14;     /* ring size wrong, or bad ghost accounting */
    }


    return 0;
}

static int check_ring(ccobject *self,const char *context)
{
#ifdef MUCH_RING_CHECKING
    int code=_check_ring(self,context);
    if(code)
    {
        /*printf(stderr,"BROKEN RING (code %d) in %s, size %d\n",code,context,PyDict_Size(self->data));*/
        PyErr_Format(PyExc_RuntimeError,"broken ring (code %d) in %s, size %d",code,context,PyDict_Size(self->data));
        return code;
    }
#endif
    return 0;
}

static int
present_in_ring(ccobject *self,CPersistentRing *target)
{
    CPersistentRing *here = self->ring_home.next;
    while(1)
    {
        if(here==target)
        {
            return 1;
        }
        if(here==&self->ring_home)
        {
            /* back to the home position, and we didnt find it */
            return 0;
        }
        here = here->next;
    }
}


static PyMappingMethods cc_as_mapping = {
  (inquiry)cc_length,		/*mp_length*/
  (binaryfunc)cc_subscript,	/*mp_subscript*/
  (objobjargproc)cc_ass_sub,	/*mp_ass_subscript*/
};

static PyTypeObject Cctype = {
  PyObject_HEAD_INIT(NULL)
  0,				/*ob_size*/
  "cPickleCache",		/*tp_name*/
  sizeof(ccobject),		/*tp_basicsize*/
  0,				/*tp_itemsize*/
  /* methods */
  (destructor)cc_dealloc,	/*tp_dealloc*/
  (printfunc)0,			/*tp_print*/
  (getattrfunc)cc_getattr,	/*tp_getattr*/
  (setattrfunc)cc_setattr,	/*tp_setattr*/
  (cmpfunc)0,			/*tp_compare*/
  (reprfunc)0,   		/*tp_repr*/
  0,				/*tp_as_number*/
  0,				/*tp_as_sequence*/
  &cc_as_mapping,		/*tp_as_mapping*/
  (hashfunc)0,			/*tp_hash*/
  (ternaryfunc)0,		/*tp_call*/
  (reprfunc)0,  		/*tp_str*/

  /* Space for future expansion */
  0L,0L,0L,0L,
  ""
};

static PyObject *
cCM_new(PyObject *self, PyObject *args)
{
  int cache_size=100, cache_age=1000;
  PyObject *jar;

  UNLESS(PyArg_ParseTuple(args, "O|ii", &jar, &cache_size, &cache_age))
      return NULL;
  return (PyObject*)newccobject(jar, cache_size,cache_age);
}

static struct PyMethodDef cCM_methods[] = {
  {"PickleCache",(PyCFunction)cCM_new,	METH_VARARGS, ""},
  {NULL,		NULL}		/* sentinel */
};

void
initcPickleCache(void)
{
  PyObject *m, *d;

  Cctype.ob_type=&PyType_Type;

  UNLESS(ExtensionClassImported) return;

  m = Py_InitModule4("cPickleCache", cCM_methods, cPickleCache_doc_string,
		     (PyObject*)NULL, PYTHON_API_VERSION);

  py_reload = PyString_InternFromString("reload");
  py__p_jar = PyString_InternFromString("_p_jar");
  py__p_changed = PyString_InternFromString("_p_changed");
  py__p_oid = PyString_InternFromString("_p_oid");

  d = PyModule_GetDict(m);

  PyDict_SetItemString(d,"cache_variant",PyString_FromString("stiff/c"));

#ifdef MUCH_RING_CHECKING
  PyDict_SetItemString(d,"MUCH_RING_CHECKING",PyInt_FromLong(1));
#else
  PyDict_SetItemString(d,"MUCH_RING_CHECKING",PyInt_FromLong(0));
#endif
}
