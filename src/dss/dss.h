/* -*- C -*-
 *
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * Data packing subsystem.
 */

#ifndef PRTE_DSS_H_
#define PRTE_DSS_H_

#include "prte_config.h"

#include "src/dss/dss_types.h"

BEGIN_C_DECLS

/* Provide a macro for determining the bool value of an prte_value_t */
#define PRTE_CHECK_BOOL(v, p)           \
    do {                                \
        if (PRTE_UNDEF == (v)->type) {  \
            (p) = true;                 \
        } else {                        \
            (p) = (v)->data.flag;       \
        }                               \
    } while(0)

/* A non-API function for something that happens in a number
 * of places throughout the code base - loading a value into
 * an prte_value_t structure
 */
PRTE_EXPORT int prte_value_load(prte_value_t *kv,
                                  void *data, prte_data_type_t type);
PRTE_EXPORT int prte_value_unload(prte_value_t *kv,
                                    void **data, prte_data_type_t type);
PRTE_EXPORT int prte_value_xfer(prte_value_t *dest,
                                  prte_value_t *src);
/**
 * Top-level interface function to pack one or more values into a
 * buffer.
 *
 * The pack function packs one or more values of a specified type into
 * the specified buffer.  The buffer must have already been
 * initialized via an PRTE_NEW or PRTE_CONSTRUCT call - otherwise, the
 * pack_value function will return an error. Providing an unsupported
 * type flag will likewise be reported as an error.
 *
 * Note that any data to be packed that is not hard type cast (i.e.,
 * not type cast to a specific size) may lose precision when unpacked
 * by a non-homogeneous recipient.  The DSS will do its best to deal
 * with heterogeneity issues between the packer and unpacker in such
 * cases. Sending a number larger than can be handled by the recipient
 * will return an error code (generated by the DSS upon unpacking) via
 * the RML upon transmission - the DSS cannot detect such errors
 * during packing.
 *
 * @param *buffer A pointer to the buffer into which the value is to
 * be packed.
 *
 * @param *src A void* pointer to the data that is to be packed. Note
 * that strings are to be passed as (char **) - i.e., the caller must
 * pass the address of the pointer to the string as the void*. This
 * allows the DSS to use a single interface function, but still allow
 * the caller to pass multiple strings in a single call.
 *
 * @param num A size_t value indicating the number of values that are
 * to be packed, beginning at the location pointed to by src. A string
 * value is counted as a single value regardless of length. The values
 * must be contiguous in memory. Arrays of pointers (e.g., string
 * arrays) should be contiguous, although (obviously) the data pointed
 * to need not be contiguous across array entries.
 *
 * @param type The type of the data to be packed - must be one of the
 * DSS defined data types.
 *
 * @retval PRTE_SUCCESS The data was packed as requested.
 *
 * @retval PRTE_ERROR(s) An appropriate PRTE error code indicating the
 * problem encountered. This error code should be handled
 * appropriately.
 *
 * @code
 * prte_buffer_t *buffer;
 * int32_t src;
 *
 * status_code = prte_dss.pack(buffer, &src, 1, PRTE_INT32);
 * @endcode
 */
typedef int (*prte_dss_pack_fn_t)(prte_buffer_t *buffer, const void *src,
                                  int32_t num_values,
                                  prte_data_type_t type);

/**
 * Unpack values from a buffer.
 *
 * The unpack function unpacks the next value (or values) of a
 * specified type from the specified buffer.
 *
 * The buffer must have already been initialized via an PRTE_NEW or
 * PRTE_CONSTRUCT call (and assumedly filled with some data) -
 * otherwise, the unpack_value function will return an
 * error. Providing an unsupported type flag will likewise be reported
 * as an error, as will specifying a data type that DOES NOT match the
 * type of the next item in the buffer. An attempt to read beyond the
 * end of the stored data held in the buffer will also return an
 * error.
 *
 * NOTE: it is possible for the buffer to be corrupted and that
 * the DSS will *think* there is a proper variable type at the
 * beginning of an unpack region - but that the value is bogus (e.g., just
 * a byte field in a string array that so happens to have a value that
 * matches the specified data type flag). Therefore, the data type error check
 * is NOT completely safe. This is true for ALL unpack functions.
 *
 *
 * Unpacking values is a "destructive" process - i.e., the values are
 * removed from the buffer, thus reducing the buffer size. It is
 * therefore not possible for the caller to re-unpack a value from the
 * same buffer.
 *
 * Warning: The caller is responsible for providing adequate memory
 * storage for the requested data. The prte_dss_peek() function is
 * provided to assist in meeting this requirement. As noted below, the user
 * must provide a parameter indicating the maximum number of values that
 * can be unpacked into the allocated memory. If more values exist in the
 * buffer than can fit into the memory storage, then the dss will unpack
 * what it can fit into that location and return an error code indicating
 * that the buffer was only partially unpacked.
 *
 * Note that any data that was not hard type cast (i.e., not type cast
 * to a specific size) when packed may lose precision when unpacked by
 * a non-homogeneous recipient.  The DSS will do its best to deal with
 * heterogeneity issues between the packer and unpacker in such
 * cases. Sending a number larger than can be handled by the recipient
 * will return an error code (generated by the DSS upon unpacking) via
 * the RML upon transmission - the DSS cannot detect such errors
 * during packing.
 *
 * @param *buffer A pointer to the buffer from which the value will be
 * extracted.
 *
 * @param *dest A void* pointer to the memory location into which the
 * data is to be stored. Note that these values will be stored
 * contiguously in memory. For strings, this pointer must be to (char
 * **) to provide a means of supporting multiple string
 * operations. The DSS unpack function will allocate memory for each
 * string in the array - the caller must only provide adequate memory
 * for the array of pointers.
 *
 * @param *num A pointer to a int32_t value indicating the maximum
 * number of values that are to be unpacked, beginning at the location
 * pointed to by src. This is provided to help protect the caller from
 * memory overrun. Note that a string
 * value is counted as a single value regardless of length.
 *
 * @note The unpack function will return the actual number of values
 * unpacked in this location.
 *
 * @param type The type of the data to be unpacked - must be one of
 * the DSS defined data types.
 *
 * @retval *max_num_values The number of values actually unpacked. In
 * most cases, this should match the maximum number provided in the
 * parameters - but in no case will it exceed the value of this
 * parameter.  Note that if you unpack fewer values than are actually
 * available, the buffer will be in an unpackable state - the dss will
 * return an error code to warn of this condition.
 *
 * @retval PRTE_SUCCESS The next item in the buffer was successfully
 * unpacked.
 *
 * @retval PRTE_ERROR(s) The unpack function returns an error code
 * under one of several conditions: (a) the number of values in the
 * item exceeds the max num provided by the caller; (b) the type of
 * the next item in the buffer does not match the type specified by
 * the caller; or (c) the unpack failed due to either an error in the
 * buffer or an attempt to read past the end of the buffer.
 *
 * @code
 * prte_buffer_t *buffer;
 * int32_t dest;
 * char **string_array;
 * int32_t num_values;
 *
 * num_values = 1;
 * status_code = prte_dss.unpack(buffer, (void*)&dest, &num_values, PRTE_INT32);
 *
 * num_values = 5;
 * string_array = malloc(num_values*sizeof(char *));
 * status_code = prte_dss.unpack(buffer, (void*)(string_array), &num_values, PRTE_STRING);
 *
 * @endcode
 */
typedef int (*prte_dss_unpack_fn_t)(prte_buffer_t *buffer, void *dest,
                                    int32_t *max_num_values,
                                    prte_data_type_t type);

/**
 * Get the type and number of values of the next item in the buffer.
 *
 * The peek function looks at the next item in the buffer and returns
 * both its type and the number of values in the item. This is a
 * non-destructive function call that does not disturb the buffer, so
 * it can be called multiple times if desired.
 *
 * @param buffer A pointer to the buffer in question.
 *
 * @param type A pointer to an prte_data_type_t variable where the
 * type of the next item in the buffer is to be stored. Caller must
 * have memory backing this location.
 *
 * @param number A pointer to a int32_t variable where the number of
 * data values in the next item is to be stored. Caller must have
 * memory backing this location.
 *
 * @retval PRTE_SUCCESS Requested info was successfully returned.
 * @retval PRTE_ERROR(s) An appropriate error code indicating the
 * problem will be returned. This should be handled appropriately by
 * the caller.
 *
 */
typedef int (*prte_dss_peek_next_item_fn_t)(prte_buffer_t *buffer,
                                            prte_data_type_t *type,
                                            int32_t *number);

/**
 * Unload the data payload from a buffer.
 *
 * The unload function provides the caller with a pointer to the data
 * payload within the buffer and the size of that payload. This allows
 * the user to directly access the payload - typically used in the RML
 * to unload the payload from the buffer for transmission.
 *
 * @note This is a destructive operation. While the payload is
 * undisturbed, the function will clear the buffer's pointers to the
 * payload. Thus, the buffer and the payload are completely separated,
 * leaving the caller free to PRTE_RELEASE the buffer.
 *
 * @param buffer A pointer to the buffer whose payload is to be
 * unloaded.
 *
 * @param payload The address to a void* pointer that is to be loaded
 * with the address of the data payload in the buffer.
 *
 * @param size The size (in bytes) of the data payload in the buffer.
 *
 * @retval PRTE_SUCCESS The request was succesfully completed.
 *
 * @retval PRTE_ERROR(s) An appropriate error code indicating the
 * problem will be returned. This should be handled appropriately by
 * the caller.
 *
 * @code
 * prte_buffer_t *buffer;
 * uint8_t *bytes;
 * int32_t size;
 *
 * status_code = prte_dss.unload(buffer, (void**)(&bytes), &size);
 * PRTE_RELEASE(buffer);
 * @endcode
 */
typedef int (*prte_dss_unload_fn_t)(prte_buffer_t *buffer,
                                    void **payload,
                                    int32_t *size);

/**
 * Load a data payload into a buffer.
 *
 * The load function allows the caller to replace the payload in a
 * buffer with one provided by the caller. If a payload already exists
 * in the buffer, the function will "free" the existing data to
 * release it, and then replace the data payload with the one provided
 * by the caller.
 *
 * @note The buffer must be allocated in advance via the PRTE_NEW
 * function call - failing to do so will cause the load function to
 * return an error code.
 *
 * @note The caller is responsible for pre-packing the provided
 * payload - the load function cannot convert to network byte order
 * any data contained in the provided payload.
 *
 * @param buffer A pointer to the buffer into which lthe payload is to
 * be loaded.
 *
 * @param payload A void* pointer to the payload to be loaded into the
 * buffer.
 *
 * @param size The size (in bytes) of the provided payload.
 *
 * @retval PRTE_SUCCESS The request was successfully completed
 *
 * @retval PRTE_ERROR(s) An appropriate error code indicating the
 * problem will be returned. This should be handled appropriately by
 * the caller.
 *
 * @code
 * prte_buffer_t *buffer;
 * uint8_t bytes;
 * int32_t size;
 *
 * buffer = PRTE_NEW(prte_buffer_t);
 * status_code = prte_dss.load(buffer, (void*)(&bytes), size);
 * @endcode
 */
typedef int (*prte_dss_load_fn_t)(prte_buffer_t *buffer,
                                  void *payload,
                                  int32_t size);


/**
 * Copy a payload from one buffer to another
 * This function will append a copy of the payload in one buffer into
 * another buffer. If the destination buffer is NOT empty, then the
 * type of the two buffers MUST match or else an
 * error will be returned. If the destination buffer IS empty, then
 * its type will be set to that of the source buffer.
 * NOTE: This is NOT a destructive procedure - the
 * source buffer's payload will remain intact, as will any pre-existing
 * payload in the destination's buffer.
 */
typedef int (*prte_dss_copy_payload_fn_t)(prte_buffer_t *dest,
                                          prte_buffer_t *src);

/**
 * DSS register function
 *
 * This function registers variables associated with the DSS system.
 */
PRTE_EXPORT int prte_dss_register_vars (void);

/**
 * DSS initialization function.
 *
 * In dynamic libraries, declared objects and functions don't get
 * loaded until called. We need to ensure that the prte_dss function
 * structure gets loaded, so we provide an "open" call that is
 * executed as part of the program startup.
 */
PRTE_EXPORT int prte_dss_open(void);

/**
 * DSS finalize function
 */
PRTE_EXPORT int prte_dss_close(void);


/**
 * Copy a data value from one location to another.
 *
 * Since registered data types can be complex structures, the system
 * needs some way to know how to copy the data from one location to
 * another (e.g., for storage in the registry). This function, which
 * can call other copy functions to build up complex data types, defines
 * the method for making a copy of the specified data type.
 *
 * @param **dest The address of a pointer into which the
 * address of the resulting data is to be stored.
 *
 * @param *src A pointer to the memory location from which the
 * data is to be copied.
 *
 * @param type The type of the data to be copied - must be one of
 * the DSS defined data types.
 *
 * @retval PRTE_SUCCESS The value was successfully copied.
 *
 * @retval PRTE_ERROR(s) An appropriate error code.
 *
 */
typedef int (*prte_dss_copy_fn_t)(void **dest, void *src, prte_data_type_t type);

/**
 * Compare two data values.
 *
 * Since registered data types can be complex structures, the system
 * needs some way to know how to compare two data values (e.g., when
 * trying to order them in some fashion). This function, which
 * can call other compare functions to build up complex data types, defines
 * the method for comparing two values of the specified data type.
 *
 * @retval -1 Indicates first value is greater than second value
 * @retval 0 Indicates two values are equal
 * @retval +1 Indicates second value is greater than first value
 */
typedef int (*prte_dss_compare_fn_t)(const void *value1, const void *value2,
                                     prte_data_type_t type);


/**
 * Print a data value.
 *
 * Since registered data types can be complex structures, the system
 * needs some way to know how to print them (i.e., convert them to a string
 * representation).
 *
 * @retval PRTE_SUCCESS The value was successfully printed.
 *
 * @retval PRTE_ERROR(s) An appropriate error code.
 */
typedef int (*prte_dss_print_fn_t)(char **output, char *prefix, void *src, prte_data_type_t type);


/**
 * Print a data value to an output stream for debugging purposes
 *
 * Uses the dss.print command to obtain a string version of the data value
 * and prints it to the designated output stream.
 *
 * @retval PRTE_SUCCESS The value was successfully printed.
 *
 * @retval PRTE_ERROR(s) An appropriate error code.
 */
typedef int (*prte_dss_dump_fn_t)(int output_stream, void *src, prte_data_type_t type);

/**
 * Register a set of data handling functions.
 *
 *  * This function registers a set of data type functions for a specific
 * type.  An integer is returned that should be used a an argument to
 * future invocations of prte_dss.pack(), prte_dss.unpack(), prte_dss.copy(),
 * and prte_dss.compare, which
 * will trigger calls to the appropriate functions.  This
 * is most useful when extending the datatypes that the dss can
 * handle; pack and unpack functions can nest calls to prte_dss.pack()
 * / prte_dss.unpack(), so defining small pack/unpack functions can be
 * used recursively to build larger types (e.g., packing/unpacking
 * structs can use calls to prte_dss.pack()/unpack() to serialize /
 * deserialize individual members). This is likewise true for the copy
 * and compare functions.
 *
 * @param pack_fn [IN] Function pointer to the pack routine
 * @param unpack_fn [IN] Function pointer to the unpack routine
 * @param copy_fn [IN] Function pointer to copy routine
 * @param compare_fn [IN] Function pointer to compare routine
 * @param print_fn [IN] Function pointer to print routine
 * @param structured [IN] Boolean indicator as to whether or not the data is structured. A true
 * value indicates that this data type is always passed via reference (i.e., a pointer to the
 * object is passed) as opposed to directly (e.g., the way an int32_t would appear)
 * @param name [IN] String name for this pair (mainly for debugging)
 * @param type [OUT] Type number for this registration
 *
 * @returns PRTE_SUCCESS upon success
 *
 */
typedef int (*prte_dss_register_fn_t)(prte_dss_pack_fn_t pack_fn,
                                    prte_dss_unpack_fn_t unpack_fn,
                                    prte_dss_copy_fn_t copy_fn,
                                    prte_dss_compare_fn_t compare_fn,
                                    prte_dss_print_fn_t print_fn,
                                    bool structured,
                                    const char *name, prte_data_type_t *type);
/*
 * This function looks up the string name corresponding to the identified
 * data type - used for debugging messages.
 */
typedef char* (*prte_dss_lookup_data_type_fn_t)(prte_data_type_t type);

/*
 * Dump the data type list - used for debugging to see what has been registered
 */
typedef void (*prte_dss_dump_data_types_fn_t)(int output);

/* return true if the data type is structured */
typedef bool (*prte_dss_structured_fn_t)(prte_data_type_t type);

/**
 * Base structure for the DSS
 *
 * Base module structure for the DSS - presents the required function
 * pointers to the calling interface.
 */
struct prte_dss_t {
    prte_dss_pack_fn_t              pack;
    prte_dss_unpack_fn_t            unpack;
    prte_dss_copy_fn_t              copy;
    prte_dss_compare_fn_t           compare;
    prte_dss_print_fn_t             print;
    prte_dss_structured_fn_t        structured;
    prte_dss_peek_next_item_fn_t    peek;
    prte_dss_unload_fn_t            unload;
    prte_dss_load_fn_t              load;
    prte_dss_copy_payload_fn_t      copy_payload;
    prte_dss_register_fn_t          register_type;
    prte_dss_lookup_data_type_fn_t  lookup_data_type;
    prte_dss_dump_data_types_fn_t   dump_data_types;
    prte_dss_dump_fn_t              dump;
};
typedef struct prte_dss_t prte_dss_t;

PRTE_EXPORT extern prte_dss_t prte_dss;  /* holds dss function pointers */

END_C_DECLS

#endif /* PRTE_DSS_H */
