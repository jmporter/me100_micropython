/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2021-22 Jonathan Hogg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#include "py/runtime.h"
#include "py/mphal.h"
#include "py/obj.h"

#if MICROPY_PY_ESP32_PCNT

#include "shared/runtime/mpirq.h"

#include "modesp32.h"
// #include "driver/pcnt.h"
#include "driver/pulse_cnt.h"

#if !MICROPY_ENABLE_FINALISER
#error "esp32.PCNT requires MICROPY_ENABLE_FINALISER."
#endif

typedef struct _esp32_pcnt_irq_obj_t {
    mp_irq_obj_t base;
    uint32_t flags;
    uint32_t trigger;
} esp32_pcnt_irq_obj_t;


typedef struct _esp32_pcnt_channel_obj_t {
    mp_obj_base_t base;
    uint32_t id; 
    pcnt_channel_handle_t channel;
    struct _esp32_pcnt_channel_obj_t *next;
} esp32_pcnt_channel_t;

typedef struct _esp32_pcnt_obj_t {
    mp_obj_base_t base;
    uint32_t id;
    pcnt_unit_handle_t unit;
    esp32_pcnt_channel_obj_t *channels;
    esp32_pcnt_irq_obj_t *irq;
    struct _esp32_pcnt_obj_t *next;
} esp32_pcnt_obj_t;


// Linked list of PCNT units.
MP_REGISTER_ROOT_POINTER(struct _esp32_pcnt_obj_t *esp32_pcnt_obj_head);

// Once off installation of the PCNT ISR service (using the default service).
// Persists across soft reset.
// static bool pcnt_isr_service_installed = false;

static mp_obj_t esp32_pcnt_deinit(mp_obj_t self_in);

void esp32_pcnt_deinit_all(void) {
    esp32_pcnt_obj_t **pcnt = &MP_STATE_PORT(esp32_pcnt_obj_head);
    while (*pcnt != NULL) {
        esp32_pcnt_deinit(MP_OBJ_FROM_PTR(*pcnt));
        *pcnt = (*pcnt)->next;
    }
}

void esp32_pcnt_unit_init_helper(esp32_pcnt_obj_t *self, size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { 
        ARG_high_limit,
        ARG_low_limit,
    };

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_low_limit,         MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_high_limit,        MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_pos_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t low_limit = -32768;
    if (args[ARG_low_limit].u_obj != MP_OBJ_NULL) {
        mp_int_t low_limit = mp_obj_get_int(args[ARG_low_limit].u_obj);
        if (low_limit < -32768 || low_limit > 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("low limit"));
        }
    }

    mp_int_t high_limit =  32767;
    if (args[ARG_high_limit].u_obj != MP_OBJ_NULL) {
        mp_int_t high_limit = mp_obj_get_int(args[ARG_high_limit].u_obj);
        if (high_limit < 0 || high_limit > 32767) {
            mp_raise_ValueError(MP_ERROR_TEXT("high limit"));
        }
    }

    pcnt_unit_config_t unit_config = { 
        .high_limit = high_limit,
        .low_limit = low_limit
    };
    pcnt_unit_handle_t pcnt_unit = NULL;
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &pcnt_unit));

    self->unit = pcnt_unit;

    
}


// STATIC void esp32_pcnt_disable_events_for_unit(esp32_pcnt_obj_t *self) {
//     if (!self->irq) {
//         return;
//     }

//     // Disable all possible events and remove the ISR.
//     for (pcnt_evt_type_t evt_type = PCNT_EVT_THRES_1; evt_type <= PCNT_EVT_ZERO; evt_type <<= 1) {
//         check_esp_err(pcnt_event_disable(self->unit, evt_type));
//     }
//     check_esp_err(pcnt_isr_handler_remove(self->unit));

//     // Clear IRQ object state.
//     self->irq->base.handler = mp_const_none;
//     self->irq->trigger = 0;
// }

static mp_obj_t esp32_pcnt_make_new(const mp_obj_type_t *type, size_t n_pos_args, size_t n_kw_args, const mp_obj_t *args) {
    if (n_pos_args < 1) { 
        mp_raise_TypeError(MP_ERROR_TEXT("id"));
    }

    mp_uint_t id = mp_obj_get_int(args[0]);
    
    esp32_pcnt_obj_t *self = MP_STATE_PORT(esp_pcnt_obj_head);
      
    while (self){
        if(self->id == id) { 
            break;
        }
        self = self->next;
    }
    
    if(!self) { 
        self = mp_obj_malloc(esp32_pcnt_obj_t, &esp32_pcnt_type);
        self->irq = NULL;
        self->channels = NULL;
        self->next = MP_STATE_PORT(esp32_pcnt_obj_head);
        MP_STATE_PORT(esp32_pcnt_obj_head) = self;
    }else { 
        esp32_pcnt_deinit(MP_OBJ_FROM_PTR(self));
    }

    mp_map_t kwargs;
    mp_map_init_fixed_table(&kw_args, n_kw_args, args + n_pos_args);
    esp32_pcnt_unit_init_helper(self, 0, args + n_pos_args, &kw_args);
       
    return MP_OBJ_FROM_PTR(self);
}



static void esp32_pcnt_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "PCNT(%u)", self->unit);
}

static mp_obj_t esp32_pcnt_add_channel(size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum{
        ARG_edge_pin,
        ARG_edge_action,
        ARG_level_pin,
        ARG_level_action
        };

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_edge_pin,      MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_level_pin,     MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_edge_action,   MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_level_action,  MP_ARG_KW_ONLY | MP_ARG_OBJ,   {.u_obj = MP_OBJ_NULL} },
    };

    esp32_pcnt_obj_t *self = pos_args[0];
    mp_uint_t id = pos_args[1];

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_pos_args -2 , pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    channel_self = self->channels;
    while(!channel_self) {
        channel_self
    }


    if(!self->channels) { 
        self->channels = mp_obj_malloc(esp32_pcnt_channel_obj_t, &mp_obj_type)
        channel =
    }
}

static mp_obj_t esp32_pcnt_deinit(mp_obj_t self_in) {
    return mp_const_none;
    
}

static MP_DEFINE_CONST_FUN_OBJ_1(esp32_pcnt_deinit_obj, esp32_pcnt_deinit);


// STATIC mp_obj_t esp32_pcnt_value(size_t n_args, const mp_obj_t *pos_args) {
//     esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);

//     // Optionally use pcnt.value(True) to clear the counter but only support a
//     // value of zero. Note: This can lead to skipped counts.
//     if (n_args == 2) {
//         if (mp_obj_get_int(pos_args[1]) != 0) {
//             mp_raise_ValueError(MP_ERROR_TEXT("value"));
//         }
//     }

//     // This loop ensures that the caller's state (as inferred from IRQs, e.g.
//     // under/overflow) corresponds to the returned value, by synchronously
//     // flushing all pending IRQs.
//     int16_t value;
//     while (true) {
//         check_esp_err(pcnt_get_counter_value(self->unit, &value));
//         if (self->irq && self->irq->flags && self->irq->base.handler != mp_const_none) {
//             // The handler must call irq.flags() to clear self->irq->base.flags,
//             // otherwise this will be an infinite loop.
//             mp_call_function_1(self->irq->base.handler, self->irq->base.parent);
//         } else {
//             break;
//         }
//     }

//     if (n_args == 2) {
//         // Value was given, and we've already checked it was zero, so clear
//         // the counter.
//         check_esp_err(pcnt_counter_clear(self->unit));
//     }

//     return MP_OBJ_NEW_SMALL_INT(value);
// }
// STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esp32_pcnt_value_obj, 1, 2, esp32_pcnt_value);

// STATIC mp_uint_t esp32_pcnt_irq_trigger(mp_obj_t self_in, mp_uint_t new_trigger) {
//     esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(self_in);
//     self->irq->trigger = new_trigger;
//     for (pcnt_evt_type_t evt_type = PCNT_EVT_THRES_1; evt_type <= PCNT_EVT_ZERO; evt_type <<= 1) {
//         if (new_trigger & evt_type) {
//             pcnt_event_enable(self->unit, evt_type);
//         } else {
//             pcnt_event_disable(self->unit, evt_type);
//         }
//     }
//     return 0;
// }

// STATIC mp_uint_t esp32_pcnt_irq_info(mp_obj_t self_in, mp_uint_t info_type) {
//     esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(self_in);
//     if (info_type == MP_IRQ_INFO_FLAGS) {
//         // Atomically get-and-clear the flags.
//         mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
//         mp_uint_t flags = self->irq->flags;
//         self->irq->flags = 0;
//         MICROPY_END_ATOMIC_SECTION(atomic_state);
//         return flags;
//     } else if (info_type == MP_IRQ_INFO_TRIGGERS) {
//         return self->irq->trigger;
//     }
//     return 0;
// }

// STATIC const mp_irq_methods_t esp32_pcnt_irq_methods = {
//     .trigger = esp32_pcnt_irq_trigger,
//     .info = esp32_pcnt_irq_info,
// };

// STATIC IRAM_ATTR void esp32_pcnt_intr_handler(void *arg) {
//     esp32_pcnt_obj_t *self = (esp32_pcnt_obj_t *)arg;
//     pcnt_unit_t unit = self->unit;
//     uint32_t status;
//     pcnt_get_event_status(unit, &status);
//     mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
//     self->irq->flags |= status;
//     MICROPY_END_ATOMIC_SECTION(atomic_state);
//     mp_irq_handler(&self->irq->base);
// }

// STATIC mp_obj_t esp32_pcnt_irq(size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
//     enum { ARG_handler, ARG_trigger };
//     static const mp_arg_t allowed_args[] = {
//         { MP_QSTR_handler,  MP_ARG_OBJ,  {.u_obj = mp_const_none} },
//         { MP_QSTR_trigger,  MP_ARG_INT,  {.u_int = PCNT_EVT_ZERO} },
//     };

//     esp32_pcnt_obj_t *self = pos_args[0];
//     mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
//     mp_arg_parse_all(n_pos_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

//     if (!self->irq) {
//         // Create IRQ object if necessary. This instance persists across a
//         // de-init.
//         self->irq = mp_obj_malloc(esp32_pcnt_irq_obj_t, &mp_irq_type);
//         self->irq->base.methods = (mp_irq_methods_t *)&esp32_pcnt_irq_methods;
//         self->irq->base.parent = MP_OBJ_FROM_PTR(self);
//         self->irq->base.ishard = false;
//         self->irq->base.handler = mp_const_none;
//         self->irq->trigger = 0;
//     }

//     if (n_pos_args > 1 || kw_args->used != 0) {
//         // Update IRQ data.

//         mp_obj_t handler = args[ARG_handler].u_obj;
//         mp_uint_t trigger = args[ARG_trigger].u_int;

//         if (trigger < PCNT_EVT_THRES_1 || trigger >= (PCNT_EVT_ZERO << 1)) {
//             mp_raise_ValueError(MP_ERROR_TEXT("trigger"));
//         }

//         if (handler != mp_const_none) {
//             self->irq->base.handler = handler;
//             self->irq->trigger = trigger;
//             pcnt_isr_handler_add(self->unit, esp32_pcnt_intr_handler, (void *)self);
//             esp32_pcnt_irq_trigger(MP_OBJ_FROM_PTR(self), trigger);
//         } else {
//             // Remove the ISR, disable all events, clear the IRQ object state.
//             esp32_pcnt_disable_events_for_unit(self);
//         }
//     }

//     return MP_OBJ_FROM_PTR(self->irq);
// }
// STATIC MP_DEFINE_CONST_FUN_OBJ_KW(esp32_pcnt_irq_obj, 1, esp32_pcnt_irq);

// STATIC mp_obj_t esp32_pcnt_start(mp_obj_t self_in) {
//     esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(self_in);
//     check_esp_err(pcnt_counter_resume(self->unit));
//     return mp_const_none;
// }
// STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_pcnt_start_obj, esp32_pcnt_start);

// STATIC mp_obj_t esp32_pcnt_stop(mp_obj_t self_in) {
//     esp32_pcnt_obj_t *self = MP_OBJ_TO_PTR(self_in);
//     check_esp_err(pcnt_counter_pause(self->unit));
//     return mp_const_none;
// }
// STATIC MP_DEFINE_CONST_FUN_OBJ_1(esp32_pcnt_stop_obj, esp32_pcnt_stop);

STATIC const mp_rom_map_elem_t esp32_pcnt_locals_dict_table[] = {
    // Methods
    // { MP_ROM_QSTR(MP_QSTR_init),            MP_ROM_PTR(&esp32_pcnt_init_obj) },
    // { MP_ROM_QSTR(MP_QSTR_value),           MP_ROM_PTR(&esp32_pcnt_value_obj) },
    // { MP_ROM_QSTR(MP_QSTR_irq),             MP_ROM_PTR(&esp32_pcnt_irq_obj) },
    // { MP_ROM_QSTR(MP_QSTR_start),           MP_ROM_PTR(&esp32_pcnt_start_obj) },
    // { MP_ROM_QSTR(MP_QSTR_stop),            MP_ROM_PTR(&esp32_pcnt_stop_obj) },
    // { MP_ROM_QSTR(MP_QSTR_deinit),          MP_ROM_PTR(&esp32_pcnt_deinit_obj) },
    // { MP_ROM_QSTR(MP_QSTR___del__),         MP_ROM_PTR(&esp32_pcnt_deinit_obj) },

    // Constants
    // { MP_ROM_QSTR(MP_QSTR_IGNORE),          MP_ROM_INT(PCNT_COUNT_DIS) },
    // { MP_ROM_QSTR(MP_QSTR_INCREMENT),       MP_ROM_INT(PCNT_COUNT_INC) },
    // { MP_ROM_QSTR(MP_QSTR_DECREMENT),       MP_ROM_INT(PCNT_COUNT_DEC) },
    // { MP_ROM_QSTR(MP_QSTR_NORMAL),          MP_ROM_INT(PCNT_MODE_KEEP) },
    // { MP_ROM_QSTR(MP_QSTR_REVERSE),         MP_ROM_INT(PCNT_MODE_REVERSE) },
    // { MP_ROM_QSTR(MP_QSTR_HOLD),            MP_ROM_INT(PCNT_MODE_DISABLE) },
    // { MP_ROM_QSTR(MP_QSTR_IRQ_ZERO),        MP_ROM_INT(PCNT_EVT_ZERO) },
    // { MP_ROM_QSTR(MP_QSTR_IRQ_THRESHOLD0),  MP_ROM_INT(PCNT_EVT_THRES_0) },
    // { MP_ROM_QSTR(MP_QSTR_IRQ_THRESHOLD1),  MP_ROM_INT(PCNT_EVT_THRES_1) },
    // { MP_ROM_QSTR(MP_QSTR_IRQ_MIN),         MP_ROM_INT(PCNT_EVT_L_LIM) },
    // { MP_ROM_QSTR(MP_QSTR_IRQ_MAX),         MP_ROM_INT(PCNT_EVT_H_LIM) },
};
static MP_DEFINE_CONST_DICT(esp32_pcnt_locals_dict, esp32_pcnt_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    esp32_pcnt_type,
    MP_QSTR_PCNT,
    MP_TYPE_FLAG_NONE,
    make_new, esp32_pcnt_make_new,
    print, esp32_pcnt_print,
    locals_dict, &esp32_pcnt_locals_dict
    );

#endif // MICROPY_PY_ESP32_PCNT