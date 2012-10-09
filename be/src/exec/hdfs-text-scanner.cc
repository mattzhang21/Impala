// Copyright (c) 2012 Cloudera, Inc. All rights reserved.

#include "codegen/llvm-codegen.h"
#include "exec/byte-stream.h"
#include "exec/delimited-text-parser.h"
#include "exec/hdfs-scan-node.h"
#include "exec/hdfs-text-scanner.h"
#include "exec/scan-range-context.h"
#include "exec/text-converter.h"
#include "exec/text-converter.inline.h"
#include "runtime/row-batch.h"
#include "runtime/runtime-state.h"
#include "util/cpu-info.h"
#include "util/debug-util.h"

using namespace boost;
using namespace impala;
using namespace llvm;
using namespace std;

const char* HdfsTextScanner::LLVM_CLASS_NAME = "class.impala::HdfsTextScanner";

HdfsTextScanner::HdfsTextScanner(HdfsScanNode* scan_node, RuntimeState* state) 
    : HdfsScanner(scan_node, state, NULL),
      boundary_mem_pool_(new MemPool()),
      boundary_row_(boundary_mem_pool_.get()),
      boundary_column_(boundary_mem_pool_.get()),
      slot_idx_(0),
      byte_buffer_ptr_(NULL),
      byte_buffer_end_(NULL),
      byte_buffer_read_size_(0),
      error_in_row_(false),
      write_tuples_fn_(NULL) {
}

HdfsTextScanner::~HdfsTextScanner() {
  COUNTER_UPDATE(scan_node_->memory_used_counter(),
      boundary_mem_pool_->peak_allocated_bytes());
}

void HdfsTextScanner::IssueInitialRanges(HdfsScanNode* scan_node, 
    const vector<HdfsFileDesc*>& files) {
  // Text just issues all ranges at once
  for (int i = 0; i < files.size(); ++i) {
    scan_node->AddDiskIoRange(files[i]);
  }
}

Status HdfsTextScanner::ProcessScanRange(ScanRangeContext* context) {
  // Reset state for new scan range
  InitNewRange(context);

  // Find the first tuple.  If eosr is true, it means we went through the entire
  // scan range without finding a single tuple.  The bytes will be picked up
  // by the scan range before.
  bool tuple_found;
  RETURN_IF_ERROR(FindFirstTuple(&tuple_found));

  if (tuple_found) {
    // Process the scan range
    int dummy_num_tuples;
    RETURN_IF_ERROR(ProcessRange(&dummy_num_tuples, false));

    // Finish up reading past the scan range
    RETURN_IF_ERROR(FinishScanRange());
  }

  // All done with this scan range.
  return Status::OK;
}

Status HdfsTextScanner::Close() {
  context_->AcquirePool(boundary_mem_pool_.get());
  scan_node_->RangeComplete();
  context_->Complete();
  return Status::OK;
}

void HdfsTextScanner::InitNewRange(ScanRangeContext* context) {
  context->set_read_past_buffer_size(NEXT_BLOCK_READ_SIZE);
  context_ = context;
  HdfsPartitionDescriptor* hdfs_partition = context_->partition_descriptor();
  
  char field_delim = hdfs_partition->field_delim();
  char collection_delim = hdfs_partition->collection_delim();
  if (scan_node_->materialized_slots().size() == 0) {
    field_delim = '\0';
    collection_delim = '\0';
  }
  
  delimited_text_parser_.reset(new DelimitedTextParser(scan_node_,
      hdfs_partition->line_delim(), field_delim, collection_delim,
      hdfs_partition->escape_char()));
  text_converter_.reset(new TextConverter(hdfs_partition->escape_char()));

  error_in_row_ = false;
  
  // Note - this initialisation relies on the assumption that N partition keys will occupy
  // entries 0 through N-1 in column_idx_to_slot_idx. If this changes, we will need
  // another layer of indirection to map text-file column indexes onto the
  // column_idx_to_slot_idx table used below.
  slot_idx_ = 0;
  
  boundary_column_.Clear();
  boundary_row_.Clear();
  delimited_text_parser_->ParserReset();

  template_tuple_ = context_->template_tuple();
  partial_tuple_empty_ = true;
  byte_buffer_ptr_ = byte_buffer_end_ = NULL;

  partial_tuple_ = reinterpret_cast<Tuple*>(
      boundary_mem_pool_->Allocate(scan_node_->tuple_desc()->byte_size()));

  // Initialize codegen fn
  // Cannot codegen if it has strings slots and we need to compact (i.e. copy) the data.
  Function* codegen_fn = scan_node_->GetCodegenFn(THdfsFileFormat::TEXT);
  if (codegen_fn == NULL) return;
  if (!scan_node_->tuple_desc()->string_slots().empty() && 
        ((hdfs_partition->escape_char() != '\0') || context_->compact_data())) {
    return;
  }
  
  write_tuples_fn_ = reinterpret_cast<WriteTuplesFn>(
      state_->llvm_codegen()->JitFunction(codegen_fn));
  VLOG(2) << "HdfsTextScanner(node_id=" << scan_node_->id()
          << ") using llvm codegend functions.";
}

Status HdfsTextScanner::FinishScanRange() {
  // For text we always need to scan past the scan range to find the next delimiter
  while (true) {
    bool eosr;
    Status status = FillByteBuffer(&eosr, NEXT_BLOCK_READ_SIZE);
    
    if (!status.ok() || byte_buffer_read_size_ == 0) {
      stringstream ss;
      if (status.IsCancelled()) return status;
      
      if (!status.ok()) {
        ss << "Read failed while trying to finish scan range: " << context_->filename() 
           << ":" << context_->file_offset() << endl << status.GetErrorMsg();
        if (state_->LogHasSpace()) state_->LogError(ss.str());
        if (state_->abort_on_error()) return Status(ss.str());
      } else if (!partial_tuple_empty_ || !boundary_column_.Empty() || 
          !boundary_row_.Empty()) {
        // Missing columns or row delimiter at end of the file is ok, fill the row in.
        char* col = boundary_column_.str().ptr;
        int num_fields = 0;
        delimited_text_parser_->FinishTuple(boundary_column_.Size(),
            &col, &num_fields, &field_locations_);

        MemPool* pool;
        TupleRow* tuple_row_mem;
        int max_tuples = context_->GetMemory(&pool, &tuple_, &tuple_row_mem);
        DCHECK_GE(max_tuples, 1);
        int tuples = WriteFields(pool, tuple_row_mem, num_fields, 1);
        DCHECK_EQ(tuples, 1);
        context_->CommitRows(tuples);
        
      }
      break;
    }

    DCHECK(eosr);

    int num_tuples;
    RETURN_IF_ERROR(ProcessRange(&num_tuples, true));
    if (num_tuples == 1) break;
    DCHECK_EQ(num_tuples, 0);
  }

  return Status::OK;
}

Status HdfsTextScanner::ProcessRange(int* num_tuples, bool past_scan_range) {
  bool eosr = past_scan_range || context_->eosr();

  while (true) {
    if (!eosr && byte_buffer_ptr_ == byte_buffer_end_) {
      RETURN_IF_ERROR(FillByteBuffer(&eosr));
    }
  
    MemPool* pool;
    TupleRow* tuple_row_mem;
    int max_tuples = context_->GetMemory(&pool, &tuple_, &tuple_row_mem);
    
    if (past_scan_range) {
      // byte_buffer_ptr_ is already set from FinishScanRange()
      max_tuples = 1;
      eosr = true;
    } 

    *num_tuples = 0;
    int num_fields = 0;
    
    DCHECK_GT(max_tuples, 0);
    
    batch_start_ptr_ = byte_buffer_ptr_;
    char* col_start = byte_buffer_ptr_;
    {
      // Parse the bytes for delimiters and store their offsets in field_locations_
      SCOPED_TIMER(parse_delimiter_timer_);
      RETURN_IF_ERROR(delimited_text_parser_->ParseFieldLocations(max_tuples,
          byte_buffer_end_ - byte_buffer_ptr_, &byte_buffer_ptr_, 
          &row_end_locations_[0],
          &field_locations_, num_tuples, &num_fields, &col_start));
    }

    // Materialize the tuples into the in memory format for this query
    int num_tuples_materialized = 0;
    if (scan_node_->materialized_slots().size() != 0 &&
        (num_fields > 0 || *num_tuples > 0)) {
      // There can be one partial tuple which returned no more fields from this buffer.
      DCHECK_LE(*num_tuples, num_fields + 1);
      if (!boundary_column_.Empty()) {
        CopyBoundaryField(&field_locations_[0], pool);
        boundary_column_.Clear();
      }
      num_tuples_materialized = WriteFields(pool, tuple_row_mem, num_fields, *num_tuples);
      DCHECK_GE(num_tuples_materialized, 0);
      RETURN_IF_ERROR(parse_status_);
      if (num_tuples > 0) {
        // If we saw any tuple delimiters, clear the boundary_row_.
        boundary_row_.Clear();
      }
    } else if (*num_tuples != 0) {
      SCOPED_TIMER(scan_node_->materialize_tuple_timer());
      // If we are doing count(*) then we return tuples only containing partition keys
      boundary_row_.Clear();
      num_tuples_materialized = WriteEmptyTuples(context_, tuple_row_mem, *num_tuples);
    }

    // Save contents that are split across buffers if we are going to return this column
    if (col_start != byte_buffer_ptr_ && delimited_text_parser_->ReturnCurrentColumn()) {
      DCHECK_EQ(byte_buffer_ptr_, byte_buffer_end_);
      boundary_column_.Append(col_start, byte_buffer_ptr_ - col_start);
      char* last_row = NULL;
      if (*num_tuples == 0) {
        last_row = batch_start_ptr_;
      } else {
        last_row = row_end_locations_[*num_tuples - 1] + 1;
      }
      boundary_row_.Append(last_row, byte_buffer_ptr_ - last_row);
    }
    
    // Commit the rows to the row batch and scan node
    if (num_tuples_materialized > 0) {
      context_->CommitRows(num_tuples_materialized);
    }

    // Done with this buffer and the scan range
    if ((byte_buffer_ptr_ == byte_buffer_end_ && eosr) || past_scan_range) {
      break;
    } 

    // Scanning was aborted by main thread
    if (context_->cancelled()) break;
  }

  return Status::OK;
}

Status HdfsTextScanner::FillByteBuffer(bool* eosr, int num_bytes) {
  *eosr = false;
  RETURN_IF_ERROR(context_->GetBytes(
      reinterpret_cast<uint8_t**>(&byte_buffer_ptr_), num_bytes, 
          &byte_buffer_read_size_, eosr));
  byte_buffer_end_ = byte_buffer_ptr_ + byte_buffer_read_size_;
  return Status::OK;
}

Status HdfsTextScanner::FindFirstTuple(bool* tuple_found) {
  *tuple_found = true;
  if (context_->file_offset() != 0) {
    *tuple_found = false;
    // Offset may not point to tuple boundary, skip ahead to the first full tuple
    // start.
    while (true) {
      bool eosr = false;
      RETURN_IF_ERROR(FillByteBuffer(&eosr));

      SCOPED_TIMER(parse_delimiter_timer_);
      int first_tuple_offset = delimited_text_parser_->FindFirstInstance(
          byte_buffer_ptr_, byte_buffer_read_size_);
      
      if (first_tuple_offset == -1) {
        // Didn't find tuple in this buffer, keep going with this scan range
        if (!eosr) continue;
      } else {
        byte_buffer_ptr_ += first_tuple_offset;
        *tuple_found = true;
      }
      break;
    }
  }
  DCHECK(delimited_text_parser_->AtTupleStart());
  return Status::OK;
}

// Codegen for materializing parsed data into tuples.  The function WriteCompleteTuple is
// codegen'd using the IRBuilder for the specific tuple description.  This function
// is then injected into the cross-compiled driving function, WriteAlignedTuples().
Function* HdfsTextScanner::Codegen(HdfsScanNode* node) {
  LlvmCodeGen* codegen = node->runtime_state()->llvm_codegen();
  if (codegen == NULL) return NULL;
  Function* write_complete_tuple_fn = CodegenWriteCompleteTuple(node, codegen);
  if (write_complete_tuple_fn == NULL) return NULL;
  return CodegenWriteAlignedTuples(node, codegen, write_complete_tuple_fn);
}

Status HdfsTextScanner::Prepare() {
  RETURN_IF_ERROR(HdfsScanner::Prepare());
  
  parse_delimiter_timer_ = ADD_COUNTER(
      scan_node_->runtime_profile(), "DelimiterParseTime", TCounterType::CPU_TICKS);

  // Allocate the scratch space for two pass parsing.  The most fields we can go
  // through in one parse pass is the batch size (tuples) * the number of fields per tuple
  // TODO: This should probably be based on L2/L3 cache sizes (as should the batch size)
  field_locations_.resize(state_->batch_size() * scan_node_->materialized_slots().size());
  row_end_locations_.resize(state_->batch_size());

  return Status::OK;
}

// TODO: remove when all scanners are updated
Status HdfsTextScanner::GetNext(RowBatch* row_batch, bool* eosr) {
  DCHECK(false);
  return Status::OK;
}

bool HdfsTextScanner::ReportTupleParseError(FieldLocation* fields, uint8_t* errors,
    int row_idx) {
  for (int i = 0; i < scan_node_->materialized_slots().size(); ++i) {
    if (errors[i]) {
      const SlotDescriptor* desc = scan_node_->materialized_slots()[i];
      ReportColumnParseError(desc, fields[i].start, fields[i].len);
      errors[i] = false;
    }
  }
  parse_status_ = ReportRowParseError(row_idx);
  return parse_status_.ok();
}

Status HdfsTextScanner::ReportRowParseError(int row_idx) {
  ++num_errors_in_file_;
  if (state_->LogHasSpace()) {
    DCHECK_LT(row_idx, row_end_locations_.size());
    char* row_end = row_end_locations_[row_idx];
    char* row_start;
    if (row_idx == 0) {
      row_start = batch_start_ptr_;
    } else {
      // Row start at 1 past the row end (i.e. the row delimiter) for the previous row
      row_start = row_end_locations_[row_idx - 1] + 1; 
    }

    stringstream ss;
    ss << "file: " << context_->filename() << endl << "record: ";
    if (!boundary_row_.Empty()) {
      // Log the beginning of the line from the previous file buffer(s)
      ss << boundary_row_.str();
    }
    // Log the erroneous line (or the suffix of a line if !boundary_line.empty()).
    ss << string(row_start, row_end - row_start);
    state_->LogError(ss.str());
  }

  if (state_->abort_on_error()) {
    state_->ReportFileErrors(context_->filename(), 1);
    return Status(state_->ErrorLog());
  }
  return Status::OK;
}


// This function writes fields in 'field_locations_' to the row_batch.  This function
// deals with tuples that straddle batches.  There are two cases:
// 1. There is already a partial tuple in flight from the previous time around.
//   This tuple can either be fully materialized (all the materialized columns have
//   been processed but we haven't seen the tuple delimiter yet) or only partially
//   materialized.  In this case num_tuples can be greater than num_fields
// 2. There is a non-fully materialized tuple at the end.  The cols that have been
//   parsed so far are written to 'tuple_' and the remained will be picked up (case 1)
//   the next time around.
int HdfsTextScanner::WriteFields(MemPool* pool, TupleRow* tuple_row, 
    int num_fields, int num_tuples) {
  SCOPED_TIMER(scan_node_->materialize_tuple_timer());

  FieldLocation* fields = &field_locations_[0];

  int num_tuples_processed = 0;
  int num_tuples_materialized = 0;
  // Write remaining fields, if any, from the previous partial tuple.
  if (slot_idx_ != 0) {
    DCHECK(tuple_ != NULL);
    int num_partial_fields = scan_node_->materialized_slots().size() - slot_idx_;
    // Corner case where there will be no materialized tuples but at least one col
    // worth of string data.  In this case, make a deep copy and reuse the byte buffer.
    bool copy_strings = num_partial_fields > num_fields;
    num_partial_fields = min(num_partial_fields, num_fields);
    WritePartialTuple(fields, num_partial_fields, copy_strings);

    // This handles case 1.  If the tuple is complete and we've found a tuple delimiter
    // this time around (i.e. num_tuples > 0), add it to the row batch.  Otherwise,
    // it will get picked up the next time around
    if (slot_idx_ == scan_node_->materialized_slots().size() && num_tuples > 0) {
      if (UNLIKELY(error_in_row_)) {
        parse_status_ = ReportRowParseError(0);
        if (!parse_status_.ok()) return 0;
        error_in_row_ = false;
      }
      boundary_row_.Clear();

      memcpy(tuple_, partial_tuple_, scan_node_->tuple_desc()->byte_size());
      partial_tuple_empty_ = true;
      tuple_row->SetTuple(scan_node_->tuple_idx(), tuple_);

      slot_idx_ = 0;
      ++num_tuples_processed;
      --num_tuples;

      if (ExecNode::EvalConjuncts(conjuncts_, num_conjuncts_, tuple_row)) {
        ++num_tuples_materialized;
        tuple_ = context_->next_tuple(tuple_);
        tuple_row = context_->next_row(tuple_row);
      }
    } 

    num_fields -= num_partial_fields;
    fields += num_partial_fields;
  }

  // Write complete tuples.  The current field, if any, is at the start of a tuple.
  if (num_tuples > 0) {
    int max_added_tuples = (scan_node_->limit() == -1) ?
          num_tuples : scan_node_->limit() - scan_node_->rows_returned();
    int tuples_returned = 0;
    // Call jitted function if possible
    if (write_tuples_fn_ != NULL) {
      tuples_returned = write_tuples_fn_(this, pool, tuple_row, 
          context_->row_byte_size(), fields, num_tuples, max_added_tuples, 
          scan_node_->materialized_slots().size(), num_tuples_processed);
    } else {
      tuples_returned = WriteAlignedTuples(pool, tuple_row, 
          context_->row_byte_size(), fields, num_tuples, max_added_tuples, 
          scan_node_->materialized_slots().size(), num_tuples_processed);
    }
    if (tuples_returned == -1) return 0;
    DCHECK_EQ(slot_idx_, 0);

    num_tuples_materialized += tuples_returned;
    num_fields -= num_tuples * scan_node_->materialized_slots().size();
    fields += num_tuples * scan_node_->materialized_slots().size();
  }

  DCHECK_GE(num_fields, 0);
  DCHECK_LE(num_fields, scan_node_->materialized_slots().size());

  // Write out the remaining slots (resulting in a partially materialized tuple)
  if (num_fields != 0) {
    DCHECK(tuple_ != NULL);
    InitTuple(context_->template_tuple(), partial_tuple_);
    // If there have been no materialized tuples at this point, copy string data
    // out of byte_buffer and reuse the byte_buffer.  The copied data can be at
    // most one tuple's worth.
    WritePartialTuple(fields, num_fields, num_tuples_materialized == 0);
    partial_tuple_empty_ = false;
  }
  DCHECK_LE(slot_idx_, scan_node_->materialized_slots().size());
  return num_tuples_materialized;
}

void HdfsTextScanner::CopyBoundaryField(FieldLocation* data, MemPool* pool) {
  const int total_len = data->len + boundary_column_.Size();
  char* str_data = reinterpret_cast<char*>(pool->Allocate(total_len));
  memcpy(str_data, boundary_column_.str().ptr, boundary_column_.Size());
  memcpy(str_data + boundary_column_.Size(), data->start, data->len);
  data->start = str_data;
  data->len = total_len;
}

int HdfsTextScanner::WritePartialTuple(FieldLocation* fields, 
    int num_fields, bool copy_strings) {
  copy_strings |= scan_node_->compact_data();
  int next_line_offset = 0;
  for (int i = 0; i < num_fields; ++i) {
    int need_escape = false;
    int len = fields[i].len;
    if (len < 0) {
      len = -len;
      need_escape = true;
    }
    next_line_offset += (len + 1);

    const SlotDescriptor* desc = scan_node_->materialized_slots()[slot_idx_];
    if (!text_converter_->WriteSlot(desc, partial_tuple_,
        fields[i].start, len, true, need_escape, boundary_mem_pool_.get())) {
      ReportColumnParseError(desc, fields[i].start, len);
      error_in_row_ = true;
    }
    ++slot_idx_;
  }
  return next_line_offset;
}

Function* HdfsTextScanner::CodegenWriteAlignedTuples(HdfsScanNode* node, 
    LlvmCodeGen* codegen, Function* write_complete_tuple_fn) {
  SCOPED_TIMER(codegen->codegen_timer());
  DCHECK(write_complete_tuple_fn != NULL);

  Function* write_tuples_fn =
      codegen->GetFunction(IRFunction::HDFS_TEXT_SCANNER_WRITE_ALIGNED_TUPLES);
  DCHECK(write_tuples_fn != NULL);

  int replaced = 0;
  write_tuples_fn = codegen->ReplaceCallSites(write_tuples_fn, false,
      write_complete_tuple_fn, "WriteCompleteTuple", &replaced);
  DCHECK_EQ(replaced, 1) << "One call site should be replaced.";
  DCHECK(write_tuples_fn != NULL);

  return codegen->FinalizeFunction(write_tuples_fn);
}
