#define __STDC_FORMAT_MACROS
#include <algorithm>
#include <cstdio>
#include <inttypes.h>
#include <stdexcept>
#include <unordered_map>

#include "Python.h"

#include "exceptions.h"
#include "hooks.h"
#include "logging.h"
#include "record_reader.h"
#include "records.h"
#include "source.h"

namespace memray::api {

using namespace tracking_api;
using namespace io;
using namespace exception;

namespace {  // unnamed

const char*
allocatorName(hooks::Allocator allocator)
{
    switch (allocator) {
        case hooks::Allocator::MALLOC:
            return "malloc";
        case hooks::Allocator::FREE:
            return "free";
        case hooks::Allocator::CALLOC:
            return "calloc";
        case hooks::Allocator::REALLOC:
            return "realloc";
        case hooks::Allocator::POSIX_MEMALIGN:
            return "posix_memalign";
        case hooks::Allocator::MEMALIGN:
            return "memalign";
        case hooks::Allocator::VALLOC:
            return "valloc";
        case hooks::Allocator::PVALLOC:
            return "pvalloc";
        case hooks::Allocator::MMAP:
            return "mmap";
        case hooks::Allocator::MUNMAP:
            return "munmap";
    }

    return nullptr;
}

}  // unnamed namespace

void
RecordReader::readHeader(HeaderRecord& header)
{
    if (!d_input->read(header.magic, sizeof(MAGIC)) || (memcmp(header.magic, MAGIC, sizeof(MAGIC)) != 0))
    {
        throw std::ios_base::failure(
                "The provided input file does not look like a binary generated by memray.");
    }
    d_input->read(reinterpret_cast<char*>(&header.version), sizeof(header.version));
    if (header.version != CURRENT_HEADER_VERSION) {
        throw std::ios_base::failure(
                "The provided input file is incompatible with this version of memray.");
    }
    header.command_line.reserve(4096);
    if (!d_input->read(reinterpret_cast<char*>(&header.native_traces), sizeof(header.native_traces))
        || !d_input->read(reinterpret_cast<char*>(&header.stats), sizeof(header.stats))
        || !d_input->getline(header.command_line, '\0'))
    {
        throw std::ios_base::failure("Failed to read input file.");
    }

    if (!d_input->read(reinterpret_cast<char*>(&header.pid), sizeof(header.pid))) {
        throw std::ios_base::failure("Failed to read tPID from input file.");
    }
    if (!d_input->read(
                reinterpret_cast<char*>(&header.python_allocator),
                sizeof(header.python_allocator))) {
        throw std::ios_base::failure("Failed to read Python allocator type from input file.");
    }
}

bool
RecordReader::readVarint(size_t* val)
{
    *val = 0;
    int shift = 0;

    while (true) {
        unsigned char next;
        if (!d_input->read(reinterpret_cast<char*>(&next), sizeof(next))) {
            return false;
        }

        *val |= (static_cast<size_t>(next & 0x7f) << shift);
        if (0 == (next & 0x80)) {
            return true;
        }

        shift += 7;
        if (shift >= 64) {
            return false;
        }
    }
}

RecordReader::RecordReader(std::unique_ptr<Source> source, bool track_stacks)
: d_input(std::move(source))
, d_track_stacks(track_stacks)
{
    readHeader(d_header);

    // Reserve some space for the different containers
    d_thread_names.reserve(16);

    if (d_track_stacks) {
        TrackerStats& stats = d_header.stats;
        d_frame_map.reserve(stats.n_frames);
        d_native_frames.reserve(d_header.native_traces ? 2048 : 0);
    }
}

void
RecordReader::close() noexcept
{
    d_input->close();
}

bool
RecordReader::isOpen() const noexcept
{
    return d_input->is_open();
}

bool
RecordReader::parseFramePush(FramePush* record)
{
    return d_input->read(reinterpret_cast<char*>(record), sizeof(*record));
}

bool
RecordReader::processFramePush(const FramePush& record)
{
    if (!d_track_stacks) {
        return true;
    }
    thread_id_t tid = d_current_thread;
    auto [it, inserted] = d_stack_traces.emplace(tid, stack_t{});
    auto& stack = it->second;
    if (inserted) {
        stack.reserve(1024);
    }
    FrameTree::index_t current_stack_id = stack.empty() ? 0 : stack.back();
    FrameTree::index_t new_stack_id = d_tree.getTraceIndex(current_stack_id, record.frame_id);
    stack.push_back(new_stack_id);
    return true;
}

bool
RecordReader::parseFramePop(FramePop* record)
{
    return d_input->read(reinterpret_cast<char*>(record), sizeof(*record));
}

bool
RecordReader::processFramePop(const FramePop& record)
{
    if (!d_track_stacks) {
        return true;
    }
    thread_id_t tid = d_current_thread;

    assert(!d_stack_traces[tid].empty());
    auto count = record.count;
    while (count) {
        --count;
        d_stack_traces[tid].pop_back();
    }
    return true;
}

bool
RecordReader::parseFrameIndex(tracking_api::pyframe_map_val_t* pyframe_val)
{
    return d_input->read(reinterpret_cast<char*>(&pyframe_val->first), sizeof(pyframe_val->first))
           && d_input->getline(pyframe_val->second.function_name, '\0')
           && d_input->getline(pyframe_val->second.filename, '\0')
           && d_input->read(
                   reinterpret_cast<char*>(&pyframe_val->second.lineno),
                   sizeof(pyframe_val->second.lineno));
}

bool
RecordReader::processFrameIndex(const tracking_api::pyframe_map_val_t& pyframe_val)
{
    if (!d_track_stacks) {
        return true;
    }
    std::lock_guard<std::mutex> lock(d_mutex);
    auto iterator = d_frame_map.insert(pyframe_val);
    if (!iterator.second) {
        throw std::runtime_error("Two entries with the same ID found!");
    }
    return true;
}

bool
RecordReader::parseNativeFrameIndex(UnresolvedNativeFrame* frame)
{
    return d_input->read(reinterpret_cast<char*>(&frame->ip), sizeof(frame->ip))
           && readVarint(&frame->index);
}

bool
RecordReader::processNativeFrameIndex(const UnresolvedNativeFrame& frame)
{
    if (!d_track_stacks) {
        return true;
    }
    std::lock_guard<std::mutex> lock(d_mutex);
    d_native_frames.emplace_back(frame);
    return true;
}

bool
RecordReader::parseAllocationRecord(AllocationRecord* record, unsigned int flags)
{
    record->allocator = static_cast<hooks::Allocator>(flags);
    return d_input->read(reinterpret_cast<char*>(&record->address), sizeof(record->address))
           && readVarint(&record->size);
}

bool
RecordReader::processAllocationRecord(const AllocationRecord& record)
{
    d_latest_allocation.tid = d_current_thread;
    d_latest_allocation.address = record.address;
    d_latest_allocation.size = record.size;
    d_latest_allocation.allocator = record.allocator;
    d_latest_allocation.native_frame_id = 0;
    if (d_track_stacks) {
        auto& stack = d_stack_traces[d_latest_allocation.tid];
        d_latest_allocation.frame_index = stack.empty() ? 0 : stack.back();
    } else {
        d_latest_allocation.frame_index = 0;
    }
    d_latest_allocation.native_segment_generation = 0;
    d_latest_allocation.n_allocations = 1;
    return true;
}

bool
RecordReader::parseNativeAllocationRecord(NativeAllocationRecord* record, unsigned int flags)
{
    record->allocator = static_cast<hooks::Allocator>(flags);
    return d_input->read(reinterpret_cast<char*>(&record->address), sizeof(record->address))
           && readVarint(&record->size) && readVarint(&record->native_frame_id);
}

bool
RecordReader::processNativeAllocationRecord(const NativeAllocationRecord& record)
{
    d_latest_allocation.tid = d_current_thread;
    d_latest_allocation.address = record.address;
    d_latest_allocation.size = record.size;
    d_latest_allocation.allocator = record.allocator;
    if (d_track_stacks) {
        d_latest_allocation.native_frame_id = record.native_frame_id;
        auto& stack = d_stack_traces[d_latest_allocation.tid];
        d_latest_allocation.frame_index = stack.empty() ? 0 : stack.back();
        d_latest_allocation.native_segment_generation = d_symbol_resolver.currentSegmentGeneration();
    } else {
        d_latest_allocation.native_frame_id = 0;
        d_latest_allocation.frame_index = 0;
        d_latest_allocation.native_segment_generation = 0;
    }
    d_latest_allocation.n_allocations = 1;
    return true;
}

bool
RecordReader::parseMemoryMapStart()
{
    // Currently nothing to do (this record type has no body)
    return true;
}

bool
RecordReader::processMemoryMapStart()
{
    std::lock_guard<std::mutex> lock(d_mutex);
    d_symbol_resolver.clearSegments();
    return true;
}

bool
RecordReader::parseSegmentHeader(std::string* filename, size_t* num_segments, uintptr_t* addr)
{
    return d_input->getline(*filename, '\0')
           && d_input->read(reinterpret_cast<char*>(num_segments), sizeof(*num_segments))
           && d_input->read(reinterpret_cast<char*>(addr), sizeof(*addr));
}

bool
RecordReader::processSegmentHeader(const std::string& filename, size_t num_segments, uintptr_t addr)
{
    std::vector<Segment> segments(num_segments);
    for (size_t i = 0; i < num_segments; i++) {
        RecordType record_type;
        if (!d_input->read(reinterpret_cast<char*>(&record_type), sizeof(record_type))
            || (record_type != RecordType::SEGMENT))
        {
            return false;
        }

        Segment segment{};
        if (!parseSegment(&segment)) {
            return false;
        }
        if (d_track_stacks) {
            segments.emplace_back(segment);
        }
    }

    if (d_track_stacks) {
        std::lock_guard<std::mutex> lock(d_mutex);
        d_symbol_resolver.addSegments(filename, addr, segments);
    }
    return true;
}

bool
RecordReader::parseSegment(Segment* segment)
{
    if (!d_input->read(reinterpret_cast<char*>(segment), sizeof(*segment))) {
        return false;
    }
    return true;
}

bool
RecordReader::parseThreadRecord(std::string* name)
{
    return d_input->getline(*name, '\0');
}

bool
RecordReader::processThreadRecord(const std::string& name)
{
    d_thread_names[d_current_thread] = name;
    return true;
}

bool
RecordReader::parseMemoryRecord(MemoryRecord* record)
{
    return d_input->read(reinterpret_cast<char*>(record), sizeof(*record));
}

bool
RecordReader::processMemoryRecord(const MemoryRecord& record)
{
    d_latest_memory_record = record;
    return true;
}

bool
RecordReader::parseContextSwitch(thread_id_t* tid)
{
    return d_input->read(reinterpret_cast<char*>(tid), sizeof(*tid));
}

bool
RecordReader::processContextSwitch(thread_id_t tid)
{
    d_current_thread = tid;
    return true;
}

RecordReader::RecordResult
RecordReader::nextRecord()
{
    while (true) {
        RecordTypeAndFlags record_type_and_flags;
        if (!d_input->read(
                    reinterpret_cast<char*>(&record_type_and_flags),
                    sizeof(record_type_and_flags))) {
            return RecordResult::END_OF_FILE;
        }

        switch (record_type_and_flags.record_type) {
            case RecordType::UNINITIALIZED: {
                // Skip it. All remaining bytes should be 0.
            } break;
            case RecordType::ALLOCATION: {
                AllocationRecord record;
                if (!parseAllocationRecord(&record, record_type_and_flags.flags)
                    || !processAllocationRecord(record)) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process allocation record";
                    return RecordResult::ERROR;
                }
                return RecordResult::ALLOCATION_RECORD;
            } break;
            case RecordType::ALLOCATION_WITH_NATIVE: {
                NativeAllocationRecord record;
                if (!parseNativeAllocationRecord(&record, record_type_and_flags.flags)
                    || !processNativeAllocationRecord(record))
                {
                    if (d_input->is_open()) {
                        LOG(ERROR) << "Failed to process allocation record with native info";
                    }
                    return RecordResult::ERROR;
                }
                return RecordResult::ALLOCATION_RECORD;
            } break;
            case RecordType::MEMORY_RECORD: {
                MemoryRecord record;
                if (!parseMemoryRecord(&record) || !processMemoryRecord(record)) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process memory record";
                    return RecordResult::ERROR;
                }
                return RecordResult::MEMORY_RECORD;
            } break;
            case RecordType::CONTEXT_SWITCH: {
                thread_id_t tid;
                if (!parseContextSwitch(&tid) || !processContextSwitch(tid)) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process context switch record";
                    return RecordResult::ERROR;
                }
            } break;
            case RecordType::FRAME_PUSH: {
                FramePush record;
                if (!parseFramePush(&record) || !processFramePush(record)) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process frame push";
                    return RecordResult::ERROR;
                }
            } break;
            case RecordType::FRAME_POP: {
                FramePop record;
                if (!parseFramePop(&record) || !processFramePop(record)) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process frame pop";
                    return RecordResult::ERROR;
                }
            } break;
            case RecordType::FRAME_INDEX: {
                tracking_api::pyframe_map_val_t record;
                if (!parseFrameIndex(&record) || !processFrameIndex(record)) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process frame index";
                    return RecordResult::ERROR;
                }
            } break;
            case RecordType::NATIVE_TRACE_INDEX: {
                UnresolvedNativeFrame record;
                if (!parseNativeFrameIndex(&record) || !processNativeFrameIndex(record)) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process native frame index";
                    return RecordResult::ERROR;
                }
            } break;
            case RecordType::MEMORY_MAP_START: {
                if (!parseMemoryMapStart() || !processMemoryMapStart()) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process memory map start";
                    return RecordResult::ERROR;
                }
            } break;
            case RecordType::SEGMENT_HEADER: {
                std::string filename;
                size_t num_segments;
                uintptr_t addr;
                if (!parseSegmentHeader(&filename, &num_segments, &addr)
                    || !processSegmentHeader(filename, num_segments, addr))
                {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process segment header";
                    return RecordResult::ERROR;
                }
            } break;
            case RecordType::THREAD_RECORD: {
                std::string name;
                if (!parseThreadRecord(&name) || !processThreadRecord(name)) {
                    if (d_input->is_open()) LOG(ERROR) << "Failed to process thread record";
                    return RecordResult::ERROR;
                }
            } break;
            default:
                if (d_input->is_open()) LOG(ERROR) << "Invalid record type";
                return RecordResult::ERROR;
        }
    }
}

// Python public APIs

PyObject*
RecordReader::Py_GetStackFrame(unsigned int index, size_t max_stacks)
{
    if (!d_track_stacks) {
        PyErr_SetString(PyExc_RuntimeError, "Stack tracking is disabled");
        return NULL;
    }
    std::lock_guard<std::mutex> lock(d_mutex);

    size_t stacks_obtained = 0;
    FrameTree::index_t current_index = index;
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        return nullptr;
    }

    while (current_index != 0 && stacks_obtained++ != max_stacks) {
        auto [frame_id, next_index] = d_tree.nextNode(current_index);
        const auto& frame = d_frame_map.at(frame_id);
        PyObject* pyframe = frame.toPythonObject(d_pystring_cache);
        if (pyframe == nullptr) {
            goto error;
        }
        int ret = PyList_Append(list, pyframe);
        Py_DECREF(pyframe);
        if (ret != 0) {
            goto error;
        }
        current_index = next_index;
    }
    return list;
error:
    Py_XDECREF(list);
    return nullptr;
}

PyObject*
RecordReader::Py_GetNativeStackFrame(FrameTree::index_t index, size_t generation, size_t max_stacks)
{
    if (!d_track_stacks) {
        PyErr_SetString(PyExc_RuntimeError, "Stack tracking is disabled");
        return NULL;
    }
    std::lock_guard<std::mutex> lock(d_mutex);

    size_t stacks_obtained = 0;
    FrameTree::index_t current_index = index;
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        return nullptr;
    }

    while (current_index != 0 && stacks_obtained++ != max_stacks) {
        auto frame = d_native_frames[current_index - 1];
        current_index = frame.index;
        auto resolved_frames = d_symbol_resolver.resolve(frame.ip, generation);
        if (!resolved_frames) {
            continue;
        }
        for (auto& native_frame : resolved_frames->frames()) {
            PyObject* pyframe = native_frame.toPythonObject(d_pystring_cache);
            if (pyframe == nullptr) {
                return nullptr;
            }
            int ret = PyList_Append(list, pyframe);
            Py_DECREF(pyframe);
            if (ret != 0) {
                goto error;
            }
        }
    }
    return list;
error:
    Py_XDECREF(list);
    return nullptr;
}

HeaderRecord
RecordReader::getHeader() const noexcept
{
    return d_header;
}

std::string
RecordReader::getThreadName(thread_id_t tid)
{
    auto it = d_thread_names.find(tid);
    if (it != d_thread_names.end()) {
        return it->second;
    }
    return "";
}

Allocation
RecordReader::getLatestAllocation() const noexcept
{
    return d_latest_allocation;
}

MemoryRecord
RecordReader::getLatestMemoryRecord() const noexcept
{
    return d_latest_memory_record;
}

PyObject*
RecordReader::dumpAllRecords()
{
    std::string python_allocator;
    switch (d_header.python_allocator) {
        case PythonAllocatorType::PYTHONALLOCATOR_PYMALLOC:
            python_allocator = "pymalloc";
            break;
        case PythonAllocatorType::PYTHONALLOCATOR_PYMALLOC_DEBUG:
            python_allocator = "pymalloc debug";
            break;
        case PythonAllocatorType::PYTHONALLOCATOR_MALLOC:
            python_allocator = "pymalloc";
            break;
        case PythonAllocatorType::PYTHONALLOCATOR_OTHER:
            python_allocator = "other";
            break;
    }
    printf("HEADER magic=%.*s version=%d native_traces=%s"
           " n_allocations=%zd n_frames=%zd start_time=%lld end_time=%lld"
           " pid=%d command_line=%s python_allocator=%s\n",
           (int)sizeof(d_header.magic),
           d_header.magic,
           d_header.version,
           d_header.native_traces ? "true" : "false",
           d_header.stats.n_allocations,
           d_header.stats.n_frames,
           d_header.stats.start_time,
           d_header.stats.end_time,
           d_header.pid,
           d_header.command_line.c_str(),
           python_allocator.c_str());

    while (true) {
        if (0 != PyErr_CheckSignals()) {
            return NULL;
        }

        RecordTypeAndFlags record_type_and_flags;
        if (!d_input->read(
                    reinterpret_cast<char*>(&record_type_and_flags),
                    sizeof(record_type_and_flags))) {
            Py_RETURN_NONE;
        }

        switch (record_type_and_flags.record_type) {
            case RecordType::UNINITIALIZED: {
                // Skip it. All remaining bytes should be 0.
            } break;
            case RecordType::ALLOCATION_WITH_NATIVE: {
                printf("ALLOCATION_WITH_NATIVE ");

                NativeAllocationRecord record;
                if (!parseNativeAllocationRecord(&record, record_type_and_flags.flags)) {
                    Py_RETURN_NONE;
                }

                const char* allocator = allocatorName(record.allocator);

                std::string unknownAllocator;
                if (!allocator) {
                    unknownAllocator =
                            "<unknown allocator " + std::to_string((int)record.allocator) + ">";
                    allocator = unknownAllocator.c_str();
                }

                printf("address=%p size=%zd allocator=%s native_frame_id=%zd\n",
                       (void*)record.address,
                       record.size,
                       allocator,
                       record.native_frame_id);
            } break;
            case RecordType::ALLOCATION: {
                printf("ALLOCATION ");

                AllocationRecord record;
                if (!parseAllocationRecord(&record, record_type_and_flags.flags)) {
                    Py_RETURN_NONE;
                }

                const char* allocator = allocatorName(record.allocator);

                std::string unknownAllocator;
                if (!allocator) {
                    unknownAllocator =
                            "<unknown allocator " + std::to_string((int)record.allocator) + ">";
                    allocator = unknownAllocator.c_str();
                }
                printf("address=%p size=%zd allocator=%s\n",
                       (void*)record.address,
                       record.size,
                       allocator);
            } break;
            case RecordType::FRAME_PUSH: {
                printf("FRAME_PUSH ");

                FramePush record;
                if (!parseFramePush(&record)) {
                    Py_RETURN_NONE;
                }

                printf("frame_id=%zd\n", record.frame_id);
            } break;
            case RecordType::FRAME_POP: {
                printf("FRAME_POP ");

                FramePop record;
                if (!parseFramePop(&record)) {
                    Py_RETURN_NONE;
                }

                printf("count=%u\n", record.count);
            } break;
            case RecordType::FRAME_INDEX: {
                printf("FRAME_ID ");

                tracking_api::pyframe_map_val_t record;
                if (!parseFrameIndex(&record)) {
                    Py_RETURN_NONE;
                }

                printf("frame_id=%zd function_name=%s filename=%s lineno=%d\n",
                       record.first,
                       record.second.function_name.c_str(),
                       record.second.filename.c_str(),
                       record.second.lineno);
            } break;
            case RecordType::NATIVE_TRACE_INDEX: {
                printf("NATIVE_FRAME_ID ");

                UnresolvedNativeFrame record;
                if (!parseNativeFrameIndex(&record)) {
                    Py_RETURN_NONE;
                }

                printf("ip=%p index=%u\n", (void*)record.ip, record.index);
            } break;
            case RecordType::MEMORY_MAP_START: {
                printf("MEMORY_MAP_START\n");
                if (!parseMemoryMapStart()) {
                    Py_RETURN_NONE;
                }
            } break;
            case RecordType::SEGMENT_HEADER: {
                printf("SEGMENT_HEADER ");

                std::string filename;
                size_t num_segments;
                uintptr_t addr;
                if (!parseSegmentHeader(&filename, &num_segments, &addr)) {
                    Py_RETURN_NONE;
                }

                printf("filename=%s num_segments=%zd addr=%p\n",
                       filename.c_str(),
                       num_segments,
                       (void*)addr);
            } break;
            case RecordType::SEGMENT: {
                printf("SEGMENT ");

                Segment record;
                if (!parseSegment(&record)) {
                    Py_RETURN_NONE;
                }

                printf("%p %" PRIxPTR "\n", (void*)record.vaddr, record.memsz);
            } break;
            case RecordType::THREAD_RECORD: {
                printf("THREAD ");

                std::string name;
                if (!parseThreadRecord(&name)) {
                    Py_RETURN_NONE;
                }

                printf("%s\n", name.c_str());
            } break;
            case RecordType::MEMORY_RECORD: {
                printf("MEMORY_RECORD ");

                MemoryRecord record;
                if (!parseMemoryRecord(&record)) {
                    Py_RETURN_NONE;
                }

                printf("time=%ld memory=%" PRIxPTR "\n", record.ms_since_epoch, record.rss);
            } break;
            case RecordType::CONTEXT_SWITCH: {
                printf("CONTEXT_SWITCH ");

                thread_id_t tid;
                if (!parseContextSwitch(&tid)) {
                    Py_RETURN_NONE;
                }

                printf("tid=%lu\n", tid);
            } break;
            default: {
                printf("UNKNOWN RECORD TYPE %d\n", (int)record_type_and_flags.record_type);
                Py_RETURN_NONE;
            } break;
        }
    }
}

}  // namespace memray::api
