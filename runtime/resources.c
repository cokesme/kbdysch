#include "kbdysch.h"

#include "internal-defs.h"
#include "invoker-utils.h"

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <syscall.h>

static uint8_t reference_watermark[WATERMARK_SIZE];

static void __attribute__((constructor))constr(void)
{
  // FF 01 FD 03 ...
  for (int i = 0; i < WATERMARK_SIZE; ++i) {
    reference_watermark[i] = (i % 2) ? i : (255 - i);
  }
}

int res_should_log_assignments(struct fuzzer_state *state)
{
  return state->constant_state.log_assigns;
}

jmp_buf *res_get_stopper_env(struct fuzzer_state *state)
{
  return &state->stopper;
}

void res_set_input_data(struct fuzzer_state *state, uint8_t *data, size_t size)
{
  if (size > MAX_INPUT_LEN) {
    fprintf(stderr, "Trying to set %zu input bytes, truncating.\n", size);
    size = MAX_INPUT_LEN;
  }
  memcpy(state->constant_state.input_buffer, data, size);
  state->constant_state.length = size;
  fprintf(stderr, "Loaded %zu bytes of input.\n", size);
}

void res_load_whole_stdin(struct fuzzer_state *state)
{
  start_forksrv();
  state->constant_state.length = read(STDIN_FILENO, state->constant_state.input_buffer, MAX_INPUT_LEN);
  if (state->constant_state.length == -1) {
    perror("Cannot read input from stdin");
    abort();
  }
  fprintf(stderr, "Read %zu bytes of input (max %u).\n", state->constant_state.length, MAX_INPUT_LEN);
}

void res_save_state(struct fuzzer_state *state)
{
  memcpy(&state->saved_state, &state->current_state, sizeof(saveable_state_t));
}

void res_restore_state(struct fuzzer_state *state, int for_partiton)
{
  memcpy(&state->current_state, &state->saved_state, sizeof(saveable_state_t));
  state->mutable_state.current_part = for_partiton;
}

static uint64_t res_rand(struct fuzzer_state *state)
{
  state->current_state.rng_state *= 17239000000001LLu; // TODO use better multiplier
  return state->current_state.rng_state >> 1; // so, may be even
}

int res_get_part_count(struct fuzzer_state *state)
{
  return state->constant_state.part_count;
}

size_t res_get_cur_offset(const struct fuzzer_state *state)
{
  return state->current_state.offset;
}

ssize_t res_get_input_length(const struct fuzzer_state *state)
{
  return state->constant_state.length;
}

void res_align_next_to(struct fuzzer_state *state, size_t alignment)
{
  state->current_state.offset = (state->current_state.offset + alignment - 1) / alignment * alignment;
}

void res_skip_bytes(struct fuzzer_state *state, size_t bytes_to_skip)
{
  state->current_state.offset += bytes_to_skip;
}

static uint8_t *get_and_consume(struct fuzzer_state *state, size_t bytes)
{
  uint8_t *result = state->constant_state.input_buffer + state->current_state.offset;
  state->current_state.offset += bytes;
  return result;
}



uint64_t res_get_uint(struct fuzzer_state *state, const char *name, size_t size)
{
  uint64_t result = 0;
  assert(size == 1 || size == 2 || size == 4 || size == 8);
  res_align_next_to(state, size);
  if (state->current_state.offset + size > state->constant_state.length) {
    state->stopper_func(state);
  }
  memcpy(&result, get_and_consume(state, size), size);
  if (name != NULL) {
    LOG_ASSIGN("%zd / %zx", (int64_t)result, (int64_t)result);
  }
  return result;
}

void res_copy_bytes(struct fuzzer_state *state, void *ptr, size_t size)
{
  if (state->saved_state.offset + size > state->constant_state.length) {
    state->stopper_func(state);
  }
  uint8_t *source_ptr = get_and_consume(state, size);
  memcpy(ptr, source_ptr, size);
}

int64_t res_get_integer_from_range(struct fuzzer_state *state, const char *name, int64_t min, int64_t max)
{
  assert(min < max);
  uint64_t range = max - min + 1;
  int64_t result;

  if (IS_U8(range))
    result = min + res_get_u8(state) % range;
  else if (IS_U16(range))
    result = min + res_get_u16(state) % range;
  else if (IS_U32(range))
    result = min + res_get_u32(state) % range;
  else
    result = min + res_get_u64(state) % range;

  LOG_ASSIGN("%zd / %zx", result, result);
  return result;
}

size_t res_decide_array_size(struct fuzzer_state *state, const char *name, size_t min, size_t max)
{
  assert((int64_t)min >= 0 && (int64_t)max >= 0);
  size_t result = (size_t)res_get_integer_from_range(state, NULL, (int64_t)min, (int64_t)max);

  LOG_ASSIGN("%zd", result);
  return result;
}

static uint32_t res_compute_hash(const uint8_t *data, size_t length)
{
  uint32_t result = 0;
  for (unsigned i = 0; i < length; ++i) {
    result = result * 17239u + data[i] * 17u;
  }
  return result;
}

void res_add_to_known_strings(struct fuzzer_state *state, const char *string)
{
  size_t length = strlen(string);
  uint32_t hash = res_compute_hash(string, length);

  for (int i = 0; i < state->mutable_state.string_count; ++i) {
    if (state->mutable_state.string_hash[i] == hash) {
      return;
    }
  }

  int index = state->mutable_state.string_count++;
  state->mutable_state.strings[index] = strdup(string);
  state->mutable_state.string_hash[index] = hash;
  state->mutable_state.string_length[index] = length;
}

void res_fill_string(struct fuzzer_state *state, const char *name, char *value)
{
  int16_t length = (int16_t) res_get_u16(state);
  if (length < 0) {
    // known string prefix
    int known_index = -length % state->mutable_state.string_count;
    int immediate_length = res_get_u8(state) % 32;
    strcpy(value, state->mutable_state.strings[known_index]);
    int known_length = state->mutable_state.string_length[known_index];
    if (known_length > MAX_STRING_LEN - immediate_length - 1)
      known_length = MAX_STRING_LEN - immediate_length - 1;
    char *immediate_string = value + known_length;
    res_copy_bytes(state, immediate_string, immediate_length);
    immediate_string[immediate_length] = '\0';
    res_add_to_known_strings(state, value);
    LOG_ASSIGN("\"%s\" (known prefix)", value);
  } else if (length <= 30) {
    // read string as-is if consuming <= 32 bytes in total
    res_copy_bytes(state, value, length);
    value[length] = '\0';
    LOG_ASSIGN("\"%s\" (verbatim)", value);
  } else {
    length %= 512;
    length %= (MAX_STRING_LEN - 1);
    for (int i = 0; i < length; ++i) {
      value[i] = 'a' + (i % 26);
    }
    value[length] = '\0';
    LOG_ASSIGN("<pattern string of length %d>", length);
  }
}

void res_fill_buffer(struct fuzzer_state *state, const char *name, buffer_t buf, uint64_t *length, direction_t dir)
{
  int32_t len = res_get_u32(state);
  if (dir == OUT) {
    len &= 511; // makes len >= 0
    // fastpath
    if (state->constant_state.part_count > 1) {
      // make buffer contents initially identical in case not fully overwritten
      memset(buf, 0, len);
    }
    *length = len;
    memcpy(buf + len, reference_watermark, WATERMARK_SIZE);
    return;
  }

  assert((uintptr_t)buf % 4 == 0);
  if (len < 0) {
    _Static_assert(MAX_STRING_LEN <= MAX_BUFFER_LEN, "MAX_STRING_LEN <= MAX_BUFFER_LEN");
    res_fill_string(state, name, buf);
    len = strlen(buf) + 1;
  } else if (len <= 28) {
    // read data as-is if consuming <= 32 bytes in total
    res_copy_bytes(state, buf, len);
    LOG_ASSIGN("<buffer of size %d, verbatim contents>", len);
  } else {
    len %= 512;
    len %= MAX_BUFFER_LEN;
    // fill with pseudo-random contents
    for (uint i = 0; i < compiler_length_value(len); i += 4) {
      uint64_t rnd = res_rand(state); // use 4 least significant bytes
      *(uint32_t *)(buf + i) = rnd & 0xffffffff;
    }
    LOG_ASSIGN("<buffer of size %d, pseudo-random contents>", len);
  }
  *length = len;
  // it is faster to allow overfill and put watermark afterwards
  memcpy(buf + len, reference_watermark, WATERMARK_SIZE);
}

static int res_create_file_name(struct fuzzer_state *state)
{
  static int no_new_files = -1;
  static char __attribute__((aligned(4))) tmp_buf[MAX_FILE_NAME_LEN + 128];
  uint64_t chain_to = res_get_u16(state) % state->current_state.file_name_count;
  int16_t component_length = res_get_u16(state);

  if (no_new_files == -1) {
    no_new_files = get_bool_knob("NO_NEW_FILES", 0);
  }

  if (no_new_files || component_length < 0) {
    // reuse existing file name
    return (int)chain_to;
  }

  component_length %= 512;

  // create new path component
  if (component_length < 28) {
    // read string as-is if consuming <= 32 bytes in total
    res_copy_bytes(state, tmp_buf, component_length);
    for (int i = 0; i < component_length; ++i) {
      if (tmp_buf[i] == '.') {
        // prevent path from escaping its partition
        // TODO relax this restriction, allowing to go up the directory tree not too high
        tmp_buf[i] = '_';
      }
    }
  } else {
    // fill with pseudo-random contents
    component_length %= MAX_FILE_NAME_LEN;
    if (get_bool_knob("SIMPLE_NAMES", 0)) {
      for (uint i = 0; i < compiler_length_value(component_length); ++i) {
        tmp_buf[i] = '0' + (i % 10);
      }
    } else {
      for (uint i = 0; i < compiler_length_value(component_length) + 4; i += 4) {
        uint64_t rnd = res_rand(state); // use 4 least significant bytes
        *(uint32_t *)(tmp_buf + i) = rnd & 0xffffffff;
      }
    }
  }
  tmp_buf[component_length] = '\0';

  // attach the newly created component to the selected existing one
  char *new_name =  malloc(MAX_FILE_NAME_LEN);
  int new_index = state->current_state.file_name_count;
  free(state->mutable_state.file_names[new_index]);
  state->mutable_state.file_names[new_index] = new_name;
  state->current_state.file_name_count += 1;
  snprintf(new_name, MAX_FILE_NAME_LEN, "%s/%s", state->mutable_state.file_names[chain_to], tmp_buf);
  return new_index;
}

void res_fill_file_name(struct fuzzer_state *state, const char *name, char *value)
{
  int index_for_result = res_create_file_name(state);
  snprintf(value, MAX_FILE_NAME_LEN, "%s/%s",
      state->partitions[state->mutable_state.current_part].mount_point,
      state->mutable_state.file_names[index_for_result]
  );
  LOG_ASSIGN("%s (index %d)", value, index_for_result);
}

int res_get_fd(struct fuzzer_state *state, const char *name)
{
  int cur_part = state->mutable_state.current_part;
  uint16_t ind = res_get_u16(state) % state->partitions[cur_part].registered_fds_count;
  int result = state->partitions[cur_part].registered_fds[ind];
  LOG_ASSIGN("<FD: %d>", result);
  return result;
}

static void crash_on_difference_int(struct fuzzer_state *state, const char *descr, const char *name, uint64_t ref, uint64_t val)
{
  if (ref != val) {
    int part = state->mutable_state.current_part;
    fprintf(stderr, "%s:\n", descr);
    fprintf(stderr, "  Name = %s\n", name);
    fprintf(stderr, "  [%s] \t %zu\n", state->partitions[0].fstype, ref);
    fprintf(stderr, "  [%s] \t %zu\n", state->partitions[part].fstype, val);
    abort();
  }
}

void res_process_integer(struct fuzzer_state *state, const char *name, uint64_t reference, uint64_t value)
{
  LOG_RETURN("%zd / %zx", value, value);
  crash_on_difference_int(state, "Return values differ", name, reference, value);
}

void res_process_errno(struct fuzzer_state *state, const char *name, uint64_t reference, uint64_t value)
{
  if (value == -EFAULT && !get_bool_knob("FAULT_IS_OK", 0)) {
    fprintf(stderr, "EFAULT is returned as %s. Possible reasons:\n", name);
    fprintf(stderr, "  * some buffers should be specifically aligned (at page boundary, for example)\n");
    fprintf(stderr, "  * some buffers should be allocated using \"guest\" mmap()\n");
    fprintf(stderr, "Try adjusting invoker description or specify FAULT_IS_OK knob.\n");
    abort();
  }
  if (value < 0 && STRERROR(state, (int)value) == STRERROR(state, 100500)) {
    fprintf(stderr, "Invalid errno:\n");
    fprintf(stderr, "  Name = %s\n", name);
    fprintf(stderr, "  [%s] \t %d\n", state->partitions[state->mutable_state.current_part].fstype, errno);
    abort();
  }
  LOG_RETURN("%s", STRERROR(state, (int)value));
  crash_on_difference_int(state, "Errno differ", name, reference, value);
}

void res_process_fd(struct fuzzer_state *state, const char *name, int reference, int value)
{
  int part = state->mutable_state.current_part;
  if ((reference < 0) != (value < 0)) {
    fprintf(stderr, "Returned FDs are inconsistent:\n");
    fprintf(stderr, "  Name = %s\n", name);
    fprintf(stderr, "  [%s] \t %d\n", state->partitions[0].fstype, reference);
    fprintf(stderr, ", [%s] \t %d\n", state->partitions[part].fstype, value);
    abort();
  }
  if (value >= 0) {
    LOG_RETURN("<FD: %d>", value);
  } else {
    LOG_RETURN("<ERR: %s>", STRERROR(state, value));
  }
  state->partitions[part].registered_fds[state->partitions[part].registered_fds_count++] = value;
}

void res_process_length(struct fuzzer_state *state, const char *name, uint64_t refLength, uint64_t length)
{
  LOG_RETURN("<length = %zu>", length);
  crash_on_difference_int(state, "Returned length differ", name, refLength, length);
}

void res_process_buffer(struct fuzzer_state *state, const char *name, buffer_t refBuf, uint64_t refLength, buffer_t buf, uint64_t length, direction_t dir)
{
  assert(refLength == length);
  if (memcmp(buf + length, reference_watermark, WATERMARK_SIZE) != 0) {
    fprintf(stderr, "Corrupted buffer watermark:\n");
    fprintf(stderr, "  Name = %s\n", name);
    fprintf(stderr, "  Buffer size = %zu\n", length);
    fprintf(stderr, "  Reference: %02x %02x %02x %02x...\n",
            reference_watermark[0], reference_watermark[1], reference_watermark[2], reference_watermark[3]);
    fprintf(stderr, "  Actual:    %02x %02x %02x %02x...\n",
            buf[length], buf[length + 1], buf[length + 2], buf[length + 3]);
    abort();
  }
  if (refBuf != buf && memcmp(refBuf, buf, length) != 0) {
    // slow path...
    uint64_t ind;
    for (ind = 0; ind < length; ++ind) {
      if (refBuf[ind] != buf[ind])
        break;
    }
    int part = state->mutable_state.current_part;
    fprintf(stderr, "Returned buffers differ:\n");
    fprintf(stderr, "  Name = %s\n", name);
    fprintf(stderr, "  Position = %zu\n", ind);
    fprintf(stderr, "  [%s] \t %02x\n", state->partitions[0].fstype, refBuf[ind] & 0xff);
    fprintf(stderr, "  [%s] \t %02x\n", state->partitions[part].fstype, buf[ind] & 0xff);
    abort();
  }
}

void res_process_file_name(struct fuzzer_state *state, const char *name, const char *reference, const char *value)
{
  // TODO
}

void res_process_string(struct fuzzer_state *state, const char *name, const char *reference, const char *value)
{
  if (reference != value && strncmp(reference, value, MAX_STRING_LEN) != 0) {
    int part = state->mutable_state.current_part;
    fprintf(stderr, "Returned strings differ:\n");
    fprintf(stderr, "  Name = %s\n", name);
    fprintf(stderr, "  [%s] \t %s\n", state->partitions[0].fstype, reference);
    fprintf(stderr, "  [%s] \t %s\n", state->partitions[part].fstype, value);
    abort();
  }
  LOG_RETURN("%s", value);
}

int res_need_recurse_into_pointees(struct fuzzer_state *state, const char *name, void *reference, void *value)
{
  // recursing even if reference == value,
  // since there can be generic validation apart from comparison
  if ((reference == NULL) != (value == NULL)) {
    int part = state->mutable_state.current_part;
    fprintf(stderr, "Returned pointers are inconsistent:\n");
    fprintf(stderr, "  Name = %s\n", name);
    fprintf(stderr, "  [%s] \t %p\n", state->partitions[0].fstype, reference);
    fprintf(stderr, "  [%s] \t %p\n", state->partitions[part].fstype, value);
    abort();
  }
  return value != NULL;
}

void res_close_all_fds(struct fuzzer_state *state)
{
  for (int part = 0; part < state->constant_state.part_count; ++part) {
    for (int i = 0; i < state->partitions[part].registered_fds_count; ++i) {
      INVOKE_SYSCALL(state, close, state->partitions[part].registered_fds[i]);
    }
    state->partitions[part].registered_fds_count = 0;
  }
}
