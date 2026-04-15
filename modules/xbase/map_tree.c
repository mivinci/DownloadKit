/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * map_tree.c - Red-black tree map implementation
 *
 * Keys are ordered by their hash value. When two keys share the same
 * hash, the equality function is used to distinguish them; colliding
 * keys are chained in a singly-linked list hanging off the tree node.
 */

#include "map_private.h"

#include <stdlib.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Internal types
 * ═══════════════════════════════════════════════════════════════════ */

XDEF_ENUM(xRBColor){
  RB_BLACK = 0,
  RB_RED   = 1,
};

/**
 * @brief Overflow entry for hash collisions within a single tree node.
 */
XDEF_STRUCT(xTreeOverflow) {
  const void    *key;
  void          *val;
  xTreeOverflow *next;
};

/**
 * @brief Red-black tree node.
 *
 * Each node holds one primary key-value pair plus an optional overflow
 * chain for hash collisions.
 */
XDEF_STRUCT(xTreeNode) {
  uint64_t       hash;
  const void    *key;
  void          *val;
  xTreeOverflow *overflow; /* collision chain (NULL when no collision) */
  xRBColor       color;
  xTreeNode     *left;
  xTreeNode     *right;
  xTreeNode     *parent;
};

XDEF_STRUCT(xMapTree) {
  xMapBase   base; /* must be first */
  xTreeNode *root;
  size_t     size; /* total key-value pairs (including overflow) */
};

/* ═══════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════ */

static inline xMapTree *self(xMap m) {
  return (xMapTree *)m;
}

/* ── Rotation ─────────────────────────────────────────────────── */

static void rotate_left(xMapTree *t, xTreeNode *x) {
  xTreeNode *y = x->right;
  x->right     = y->left;
  if (y->left) y->left->parent = x;
  y->parent = x->parent;
  if (!x->parent)
    t->root = y;
  else if (x == x->parent->left)
    x->parent->left = y;
  else
    x->parent->right = y;
  y->left   = x;
  x->parent = y;
}

static void rotate_right(xMapTree *t, xTreeNode *x) {
  xTreeNode *y = x->left;
  x->left      = y->right;
  if (y->right) y->right->parent = x;
  y->parent = x->parent;
  if (!x->parent)
    t->root = y;
  else if (x == x->parent->right)
    x->parent->right = y;
  else
    x->parent->left = y;
  y->right  = x;
  x->parent = y;
}

/* ── Insert fixup ─────────────────────────────────────────────── */

static void insert_fixup(xMapTree *t, xTreeNode *z) {
  while (z->parent && z->parent->color == RB_RED) {
    if (z->parent == z->parent->parent->left) {
      xTreeNode *y = z->parent->parent->right; /* uncle */
      if (y && y->color == RB_RED) {
        /* Case 1: uncle is red */
        z->parent->color         = RB_BLACK;
        y->color                 = RB_BLACK;
        z->parent->parent->color = RB_RED;
        z                        = z->parent->parent;
      } else {
        if (z == z->parent->right) {
          /* Case 2: z is right child */
          z = z->parent;
          rotate_left(t, z);
        }
        /* Case 3: z is left child */
        z->parent->color         = RB_BLACK;
        z->parent->parent->color = RB_RED;
        rotate_right(t, z->parent->parent);
      }
    } else {
      /* Mirror: parent is right child of grandparent */
      xTreeNode *y = z->parent->parent->left; /* uncle */
      if (y && y->color == RB_RED) {
        z->parent->color         = RB_BLACK;
        y->color                 = RB_BLACK;
        z->parent->parent->color = RB_RED;
        z                        = z->parent->parent;
      } else {
        if (z == z->parent->left) {
          z = z->parent;
          rotate_right(t, z);
        }
        z->parent->color         = RB_BLACK;
        z->parent->parent->color = RB_RED;
        rotate_left(t, z->parent->parent);
      }
    }
  }
  t->root->color = RB_BLACK;
}

/* ── Delete fixup ─────────────────────────────────────────────── */

static void delete_fixup(xMapTree *t, xTreeNode *x, xTreeNode *x_parent) {
  while (x != t->root && (!x || x->color == RB_BLACK)) {
    if (x == x_parent->left) {
      xTreeNode *w = x_parent->right;
      if (w && w->color == RB_RED) {
        /* Case 1 */
        w->color        = RB_BLACK;
        x_parent->color = RB_RED;
        rotate_left(t, x_parent);
        w = x_parent->right;
      }
      if ((!w->left || w->left->color == RB_BLACK) &&
          (!w->right || w->right->color == RB_BLACK)) {
        /* Case 2 */
        w->color = RB_RED;
        x        = x_parent;
        x_parent = x->parent;
      } else {
        if (!w->right || w->right->color == RB_BLACK) {
          /* Case 3 */
          if (w->left) w->left->color = RB_BLACK;
          w->color = RB_RED;
          rotate_right(t, w);
          w = x_parent->right;
        }
        /* Case 4 */
        w->color        = x_parent->color;
        x_parent->color = RB_BLACK;
        if (w->right) w->right->color = RB_BLACK;
        rotate_left(t, x_parent);
        x = t->root;
      }
    } else {
      /* Mirror */
      xTreeNode *w = x_parent->left;
      if (w && w->color == RB_RED) {
        w->color        = RB_BLACK;
        x_parent->color = RB_RED;
        rotate_right(t, x_parent);
        w = x_parent->left;
      }
      if ((!w->right || w->right->color == RB_BLACK) &&
          (!w->left || w->left->color == RB_BLACK)) {
        w->color = RB_RED;
        x        = x_parent;
        x_parent = x->parent;
      } else {
        if (!w->left || w->left->color == RB_BLACK) {
          if (w->right) w->right->color = RB_BLACK;
          w->color = RB_RED;
          rotate_left(t, w);
          w = x_parent->left;
        }
        w->color        = x_parent->color;
        x_parent->color = RB_BLACK;
        if (w->left) w->left->color = RB_BLACK;
        rotate_right(t, x_parent);
        x = t->root;
      }
    }
  }
  if (x) x->color = RB_BLACK;
}

/* ── Transplant ───────────────────────────────────────────────── */

static void transplant(xMapTree *t, xTreeNode *u, xTreeNode *v) {
  if (!u->parent)
    t->root = v;
  else if (u == u->parent->left)
    u->parent->left = v;
  else
    u->parent->right = v;
  if (v) v->parent = u->parent;
}

/* ── Tree minimum ─────────────────────────────────────────────── */

static xTreeNode *tree_min(xTreeNode *n) {
  while (n->left)
    n = n->left;
  return n;
}

/* ── Find node by hash ────────────────────────────────────────── */

static xTreeNode *find_node(xMapTree *t, uint64_t h) {
  xTreeNode *n = t->root;
  while (n) {
    if (h < n->hash)
      n = n->left;
    else if (h > n->hash)
      n = n->right;
    else
      return n; /* hash match */
  }
  return NULL;
}

/* ── Free a node and its overflow chain ───────────────────────── */

static void free_node(xTreeNode *n) {
  xTreeOverflow *o = n->overflow;
  while (o) {
    xTreeOverflow *next = o->next;
    free(o);
    o = next;
  }
  free(n);
}

/* ── Recursive destroy ────────────────────────────────────────── */

static void destroy_subtree(xTreeNode *n) {
  if (!n) return;
  destroy_subtree(n->left);
  destroy_subtree(n->right);
  free_node(n);
}

/* ── In-order iteration ───────────────────────────────────────── */

static bool iterate_subtree(xTreeNode *n, xMapIterFunc fn, void *arg) {
  if (!n) return true;
  if (!iterate_subtree(n->left, fn, arg)) return false;

  /* Visit primary entry */
  if (!fn(n->key, n->val, arg)) return false;

  /* Visit overflow chain */
  for (xTreeOverflow *o = n->overflow; o; o = o->next) {
    if (!fn(o->key, o->val, arg)) return false;
  }

  return iterate_subtree(n->right, fn, arg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  VTable implementations
 * ═══════════════════════════════════════════════════════════════════ */

static xErrno tree_set(xMap m, const void *key, void *val) {
  xMapTree *t = self(m);
  uint64_t  h = t->base.hash(key);

  /* ── Try to find an existing node with the same hash ── */
  xTreeNode *parent = NULL;
  xTreeNode *cur    = t->root;
  int        dir    = 0; /* -1 left, +1 right */

  while (cur) {
    parent = cur;
    if (h < cur->hash) {
      cur = cur->left;
      dir = -1;
    } else if (h > cur->hash) {
      cur = cur->right;
      dir = 1;
    } else {
      /* Hash match — check primary key */
      if (t->base.eq(cur->key, key)) {
        cur->val = val;
        return xErrno_Ok;
      }
      /* Check overflow chain */
      for (xTreeOverflow *o = cur->overflow; o; o = o->next) {
        if (t->base.eq(o->key, key)) {
          o->val = val;
          return xErrno_Ok;
        }
      }
      /* Hash collision with a new key — append to overflow */
      xTreeOverflow *o = (xTreeOverflow *)malloc(sizeof(xTreeOverflow));
      if (!o) return xErrno_NoMemory;
      o->key        = key;
      o->val        = val;
      o->next       = cur->overflow;
      cur->overflow = o;
      t->size++;
      return xErrno_Ok;
    }
  }

  /* ── Insert a new tree node ── */
  xTreeNode *z = (xTreeNode *)calloc(1, sizeof(xTreeNode));
  if (!z) return xErrno_NoMemory;

  z->hash   = h;
  z->key    = key;
  z->val    = val;
  z->color  = RB_RED;
  z->parent = parent;

  if (!parent)
    t->root = z;
  else if (dir < 0)
    parent->left = z;
  else
    parent->right = z;

  insert_fixup(t, z);
  t->size++;
  return xErrno_Ok;
}

static void *tree_get(xMap m, const void *key) {
  xMapTree  *t = self(m);
  uint64_t   h = t->base.hash(key);
  xTreeNode *n = find_node(t, h);
  if (!n) return NULL;

  if (t->base.eq(n->key, key)) return n->val;

  for (xTreeOverflow *o = n->overflow; o; o = o->next) {
    if (t->base.eq(o->key, key)) return o->val;
  }
  return NULL;
}

static void *tree_del(xMap m, const void *key) {
  xMapTree  *t = self(m);
  uint64_t   h = t->base.hash(key);
  xTreeNode *n = find_node(t, h);
  if (!n) return NULL;

  /* ── Check overflow chain first ── */
  if (!t->base.eq(n->key, key)) {
    xTreeOverflow *prev = NULL;
    for (xTreeOverflow *o = n->overflow; o; prev = o, o = o->next) {
      if (t->base.eq(o->key, key)) {
        void *val = o->val;
        if (prev)
          prev->next = o->next;
        else
          n->overflow = o->next;
        free(o);
        t->size--;
        return val;
      }
    }
    return NULL; /* key not found */
  }

  /* ── Primary key matches — need to handle tree node removal ── */
  void *val = n->val;

  /* If there's an overflow entry, promote it to primary instead of
   * removing the tree node (avoids expensive RB fixup). */
  if (n->overflow) {
    xTreeOverflow *o = n->overflow;
    n->key           = o->key;
    n->val           = o->val;
    n->overflow      = o->next;
    free(o);
    t->size--;
    return val;
  }

  /* ── Standard RB-tree node deletion ── */
  xTreeNode *y        = n;
  xRBColor   y_orig   = y->color;
  xTreeNode *x        = NULL;
  xTreeNode *x_parent = NULL;

  if (!n->left) {
    x        = n->right;
    x_parent = n->parent;
    transplant(t, n, n->right);
  } else if (!n->right) {
    x        = n->left;
    x_parent = n->parent;
    transplant(t, n, n->left);
  } else {
    /* Two children: replace with in-order successor */
    y      = tree_min(n->right);
    y_orig = y->color;
    x      = y->right;

    if (y->parent == n) {
      x_parent = y;
    } else {
      x_parent = y->parent;
      transplant(t, y, y->right);
      y->right         = n->right;
      y->right->parent = y;
    }
    transplant(t, n, y);
    y->left         = n->left;
    y->left->parent = y;
    y->color        = n->color;
  }

  free(n);
  t->size--;

  if (y_orig == RB_BLACK) {
    delete_fixup(t, x, x_parent);
  }

  return val;
}

static size_t tree_len(xMap m) {
  return self(m)->size;
}

static void tree_iterate(xMap m, xMapIterFunc fn, void *arg) {
  iterate_subtree(self(m)->root, fn, arg);
}

static void tree_destroy(xMap m) {
  xMapTree *t = self(m);
  destroy_subtree(t->root);
  free(t);
}

/* ═══════════════════════════════════════════════════════════════════
 *  VTable instance
 * ═══════════════════════════════════════════════════════════════════ */

static const xMapVTable tree_vtable = {
  .set     = tree_set,
  .get     = tree_get,
  .del     = tree_del,
  .len     = tree_len,
  .iterate = tree_iterate,
  .destroy = tree_destroy,
};

/* ═══════════════════════════════════════════════════════════════════
 *  Constructor
 * ═══════════════════════════════════════════════════════════════════ */

xMap xMapTreeCreate(size_t cap, xMapHashFunc hash, xMapEqFunc eq) {
  (void)cap; /* tree does not pre-allocate */

  xMapTree *t = (xMapTree *)calloc(1, sizeof(xMapTree));
  if (!t) return NULL;

  t->base.vtable = &tree_vtable;
  t->base.hash   = hash;
  t->base.eq     = eq;
  t->root        = NULL;
  t->size        = 0;

  return (xMap)t;
}
