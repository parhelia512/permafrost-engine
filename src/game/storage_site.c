/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include "storage_site.h"
#include "game_private.h"
#include "../ui.h"
#include "../event.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mpool.h"
#include "../lib/public/khash.h"
#include "../lib/public/string_intern.h"

#include <assert.h>

static void *pmalloc(size_t size);
static void *pcalloc(size_t n, size_t size);
static void *prealloc(void *ptr, size_t size);
static void  pfree(void *ptr);

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  pmalloc
#define kcalloc  pcalloc
#define krealloc prealloc
#define kfree    pfree

#define ARR_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MAX(a, b)   ((a) > (b) ? (a) : (b))
#define MIN(a, b)   ((a) < (b) ? (a) : (b))

KHASH_MAP_INIT_STR(int, int)

struct ss_state{
    kh_int_t              *capacity;
    kh_int_t              *curr;
    kh_int_t              *desired;
    struct ss_delta_event  last_change;
};

typedef char buff_t[512];

MPOOL_TYPE(buff, buff_t)
MPOOL_PROTOTYPES(static, buff, buff_t)
MPOOL_IMPL(static, buff, buff_t)

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  malloc
#define kcalloc  calloc
#define krealloc realloc
#define kfree    free

KHASH_MAP_INIT_INT(state, struct ss_state)
KHASH_MAP_INIT_STR(res, int)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static mp_buff_t        s_mpool;
static khash_t(stridx) *s_stridx;
static mp_strbuff_t     s_stringpool;
static khash_t(state)  *s_entity_state_table;
static khash_t(res)    *s_global_resource_tables[MAX_FACTIONS];
static khash_t(res)    *s_global_capacity_tables[MAX_FACTIONS];

static struct nk_style_item s_bg_style = {0};
static struct nk_color      s_border_clr = {0};
static struct nk_color      s_font_clr = {0};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *pmalloc(size_t size)
{
    mp_ref_t ref = mp_buff_alloc(&s_mpool);
    if(ref == 0)
        return NULL;
    return mp_buff_entry(&s_mpool, ref);
}

static void *pcalloc(size_t n, size_t size)
{
    void *ret = pmalloc(n * size);
    if(!ret)
        return NULL;
    memset(ret, 0, n * size);
    return ret;
}

static void *prealloc(void *ptr, size_t size)
{
    if(!ptr)
        return pmalloc(size);
    if(size <= sizeof(buff_t))
        return ptr;
    return NULL;
}

static void pfree(void *ptr)
{
    if(!ptr)
        return;
    mp_ref_t ref = mp_buff_ref(&s_mpool, ptr);
    mp_buff_free(&s_mpool, ref);
}

static struct ss_state *ss_state_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static bool ss_state_set(uint32_t uid, struct ss_state hs)
{
    int status;
    khiter_t k = kh_put(state, s_entity_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_entity_state_table, k) = hs;
    return true;
}

static void ss_state_remove(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static void ss_state_destroy(struct ss_state *hs)
{
    kh_destroy(int, hs->capacity);
    kh_destroy(int, hs->curr);
    kh_destroy(int, hs->desired);
}

static bool ss_state_init(struct ss_state *hs)
{
    hs->capacity = kh_init(int);
    hs->curr = kh_init(int);
    hs->desired = kh_init(int);

    if(!hs->capacity || !hs->curr) {

        ss_state_destroy(hs);
        return false;
    }
    hs->last_change = (struct ss_delta_event){0};
    return true;
}

static int compare_keys(const void *a, const void *b)
{
    char *stra = *(char**)a;
    char *strb = *(char**)b;
    return strcmp(stra, strb);
}

static size_t ss_get_keys(struct ss_state *hs, const char **out, size_t maxout)
{
    size_t ret = 0;

    const char *key;
    int amount;

    kh_foreach(hs->capacity, key, amount, {
        if(ret == maxout)
            break;
        if(amount == 0)
            continue;
        out[ret++] = key;
    });

    qsort(out, ret, sizeof(char*), compare_keys);
    return ret;
}

static bool ss_state_set_key(khash_t(int) *table, const char *name, int val)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(int, table, key);
    if(k != kh_end(table)) {
        kh_value(table, k) = val;
        return true;
    }

    int status;
    k = kh_put(int, table, key, &status);
    if(status == -1)
        return false;

    assert(status == 1);
    kh_value(table, k) = val;
    return true;
}

static bool ss_state_get_key(khash_t(int) *table, const char *name, int *out)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(int, table, key);
    if(k == kh_end(table))
        return false;
    *out = kh_value(table, k);
    return true;
}

static bool update_res_delta(const char *rname, int delta, int faction_id)
{
    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(res, s_global_resource_tables[faction_id], key);
    if(k != kh_end(s_global_resource_tables[faction_id])) {
        int val = kh_value(s_global_resource_tables[faction_id], k);
        val += delta;
        kh_value(s_global_resource_tables[faction_id], k) = val;
        return true;
    }

    int status;
    k = kh_put(res, s_global_resource_tables[faction_id], key, &status);
    if(status == -1)
        return false;

    assert(status == 1);
    kh_value(s_global_resource_tables[faction_id], k) = delta;
    return true;
}

static bool update_cap_delta(const char *rname, int delta, int faction_id)
{
    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(res, s_global_capacity_tables[faction_id], key);
    if(k != kh_end(s_global_capacity_tables[faction_id])) {
        int val = kh_value(s_global_capacity_tables[faction_id], k);
        val += delta;
        kh_value(s_global_capacity_tables[faction_id], k) = val;
        return true;
    }

    int status;
    k = kh_put(res, s_global_capacity_tables[faction_id], key, &status);
    if(status == -1)
        return false;

    assert(status == 1);
    kh_value(s_global_capacity_tables[faction_id], k) = delta;
    return true;
}

static void constrain_desired(struct ss_state *ss, const char *rname)
{
    int cap = 0, desired = 0;
    ss_state_get_key(ss->capacity, rname, &cap);
    ss_state_get_key(ss->desired, rname, &desired);

    desired = MIN(desired, cap);
    desired = MAX(desired, 0);
    ss_state_set_key(ss->desired, rname, desired);
}

static void on_update_ui(void *user, void *event)
{
    uint32_t key;
    struct ss_state curr;
    struct nk_context *ctx = UI_GetContext();

    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, s_bg_style);
    nk_style_push_color(ctx, &ctx->style.window.border_color, s_border_clr);

    kh_foreach(s_entity_state_table, key, curr, {

        char name[256];
        pf_snprintf(name, sizeof(name), "__storage_site__.%x", key);

        const struct entity *ent = G_EntityForUID(key);
        vec2_t ss_pos = Entity_TopScreenPos(ent);

        const int width = 224;
        const int height = MIN(kh_size(curr.capacity), 16) * 20 + 4;
        const vec2_t pos = (vec2_t){ss_pos.x - width/2, ss_pos.y + 20};
        const int flags = NK_WINDOW_NOT_INTERACTIVE | NK_WINDOW_BORDER | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR;

        const vec2_t vres = (vec2_t){1920, 1080};
        const vec2_t adj_vres = UI_ArAdjustedVRes(vres);

        struct rect adj_bounds = UI_BoundsForAspectRatio(
            (struct rect){pos.x, pos.y, width, height}, 
            vres, adj_vres, ANCHOR_DEFAULT
        );

        const char *names[16];
        size_t nnames = ss_get_keys(&curr, names, ARR_SIZE(names));

        if(nnames == 0)
            continue;

        if(nk_begin_with_vres(ctx, name, 
            (struct nk_rect){adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h}, 
            flags, (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

            for(int i = 0; i < nnames; i++) {

                char curr[5], cap[5], desired[7];
                pf_snprintf(curr, sizeof(curr), "%4d", G_StorageSite_GetCurr(key, names[i]));
                pf_snprintf(cap, sizeof(cap), "%4d", G_StorageSite_GetCapacity(key, names[i]));
                pf_snprintf(desired, sizeof(desired), "(%4d)", G_StorageSite_GetDesired(key, names[i]));

                nk_layout_row_begin(ctx, NK_DYNAMIC, 16, 2);
                nk_layout_row_push(ctx, 0.30f);
                nk_label_colored(ctx, names[i], NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 0.20f);
                nk_label_colored(ctx, curr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 0.05f);
                nk_label_colored(ctx, "/", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 0.20f);
                nk_label_colored(ctx, cap, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 0.30f);
                nk_label_colored(ctx, desired, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
            }
        }
        nk_end(ctx);
    });

    nk_style_pop_style_item(ctx);
    nk_style_pop_color(ctx);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_StorageSite_Init(void)
{
    mp_buff_init(&s_mpool);

    if(!mp_buff_reserve(&s_mpool, 4096 * 3))
        goto fail_mpool; 
    if(!(s_entity_state_table = kh_init(state)))
        goto fail_table;
    if(0 != kh_resize(state, s_entity_state_table, 4096))
        goto fail_res;

    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(s_global_resource_tables[i] = kh_init(res))) {
            for(--i; i; i--)
                kh_destroy(res, s_global_resource_tables[i]);
            goto fail_res;
        }
    }

    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(s_global_capacity_tables[i] = kh_init(res))) {
            for(--i; i; i--)
                kh_destroy(res, s_global_capacity_tables[i]);
            goto fail_cap;
        }
    }

    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_strintern;

    struct nk_context ctx;
    nk_style_default(&ctx);

    s_bg_style = ctx.style.window.fixed_background;
    s_border_clr = ctx.style.window.border_color;
    s_font_clr = ctx.style.text.color;

    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
    return true;

fail_strintern:
    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_capacity_tables[i]);
fail_cap:
    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_resource_tables[i]);
fail_res:
    kh_destroy(state, s_entity_state_table);
fail_table:
    mp_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_StorageSite_Shutdown(void)
{
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);

    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_capacity_tables[i]);
    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_resource_tables[i]);

    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
    mp_buff_destroy(&s_mpool);
}

bool G_StorageSite_AddEntity(const struct entity *ent)
{
    struct ss_state ss;
    if(!ss_state_init(&ss))
        return false;
    if(!ss_state_set(ent->uid, ss))
        return false;
    return true;
}

void G_StorageSite_RemoveEntity(const struct entity *ent)
{
    struct ss_state *ss = ss_state_get(ent->uid);
    if(!ss)
        return;

    const char *key;
    int amount;

    kh_foreach(ss->curr, key, amount, {
        update_res_delta(key, -amount, ent->faction_id);
    });
    kh_foreach(ss->capacity, key, amount, {
        update_cap_delta(key, -amount, ent->faction_id);
    });

    ss_state_destroy(ss);
    ss_state_remove(ent->uid);
}

bool G_StorageSite_SetCapacity(const struct entity *ent, const char *rname, int max)
{
    struct ss_state *ss = ss_state_get(ent->uid);
    assert(ss);

    int prev = 0;
    ss_state_get_key(ss->curr, rname, &prev);
    int delta = max - prev;
    update_cap_delta(rname, delta, ent->faction_id);

    bool ret = ss_state_set_key(ss->capacity, rname, max);
    constrain_desired(ss, rname);
    return ret;
}

int G_StorageSite_GetCapacity(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    ss_state_get_key(ss->capacity, rname, &ret);
    return ret;
}

bool G_StorageSite_SetCurr(const struct entity *ent, const char *rname, int curr)
{
    struct ss_state *ss = ss_state_get(ent->uid);
    assert(ss);

    int prev = 0;
    ss_state_get_key(ss->curr, rname, &prev);
    int delta = curr - prev;
    update_res_delta(rname, delta, ent->faction_id);

    if(delta) {
        ss->last_change = (struct ss_delta_event){
            .name = si_intern(rname, &s_stringpool, s_stridx),
            .delta = delta
        };
        E_Entity_Notify(EVENT_STORAGE_SITE_AMOUNT_CHANGED, ent->uid, &ss->last_change, ES_ENGINE);
    }

    return ss_state_set_key(ss->curr, rname, curr);
}

int G_StorageSite_GetCurr(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    ss_state_get_key(ss->curr, rname, &ret);
    return ret;
}

bool G_StorageSite_SetDesired(uint32_t uid, const char *rname, int des)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    bool ret = ss_state_set_key(ss->desired, rname, des);
    constrain_desired(ss, rname);
    return ret;
}

int G_StorageSite_GetDesired(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    ss_state_get_key(ss->desired, rname, &ret);
    return ret;
}

int G_StorageSite_GetPlayerStored(const char *rname)
{
    int ret = 0;
    uint16_t pfacs = G_GetPlayerControlledFactions();

    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(pfacs & (0x1 << i)))
            continue;
        khiter_t k = kh_get(res, s_global_resource_tables[i], rname);
        if(k == kh_end(s_global_resource_tables[i]))
            continue;
        ret += kh_value(s_global_resource_tables[i], k);
    }
    return ret;
}

int G_StorageSite_GetPlayerCapacity(const char *rname)
{
    int ret = 0;
    uint16_t pfacs = G_GetPlayerControlledFactions();

    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(pfacs & (0x1 << i)))
            continue;
        khiter_t k = kh_get(res, s_global_capacity_tables[i], rname);
        if(k == kh_end(s_global_capacity_tables[i]))
            continue;
        ret += kh_value(s_global_capacity_tables[i], k);
    }
    return ret;
}

int G_StorageSite_GetStorableResources(uint32_t uid, size_t maxout, const char *out[static maxout])
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    return ss_get_keys(ss, out, maxout);
}

void G_StorageSite_SetFontColor(const struct nk_color *clr)
{
    s_font_clr = *clr;
}

void G_StorageSite_SetBorderColor(const struct nk_color *clr)
{
    s_border_clr = *clr;
}

void G_StorageSite_SetBackgroundStyle(const struct nk_style_item *style)
{
    s_bg_style = *style;
}

