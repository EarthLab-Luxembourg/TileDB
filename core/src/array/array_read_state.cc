/**
 * @file   array_read_state.cc
 * @author Stavros Papadopoulos <stavrosp@csail.mit.edu>
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2015 Stavros Papadopoulos <stavrosp@csail.mit.edu>
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
 *
 * @section DESCRIPTION
 *
 * This file implements the ArrayReadState class.
 */

#include "array_read_state.h"
#include <cassert>
#include <cmath>

/* ****************************** */
/*             MACROS             */
/* ****************************** */

#if VERBOSE == 1
#  define PRINT_ERROR(x) std::cerr << "[TileDB] Error: " << x << ".\n" 
#  define PRINT_WARNING(x) std::cerr << "[TileDB] Warning: " \
                                     << x << ".\n"
#elif VERBOSE == 2
#  define PRINT_ERROR(x) std::cerr << "[TileDB::ReadState] Error: " \
                                   << x << ".\n" 
#  define PRINT_WARNING(x) std::cerr << "[TileDB::ReadState] Warning: " \
                                     << x << ".\n"
#else
#  define PRINT_ERROR(x) do { } while(0) 
#  define PRINT_WARNING(x) do { } while(0) 
#endif

/* ****************************** */
/*   CONSTRUCTORS & DESTRUCTORS   */
/* ****************************** */

ArrayReadState::ArrayReadState(
    const Array* array)
    : array_(array) {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int attribute_num = array_schema->attribute_num();

  // Initializations
  tile_done_.resize(attribute_num);
  max_overlap_range_ = NULL;
  range_global_tile_coords_ = NULL;
  range_global_tile_domain_ = NULL;
  fragment_cell_pos_ranges_pos_.resize(attribute_num+1);
  fragment_cell_pos_ranges_vec_pos_.resize(attribute_num+1);
  for(int i=0; i<attribute_num+1; ++i) {
    fragment_cell_pos_ranges_pos_[i] = 0;
    fragment_cell_pos_ranges_vec_pos_[i] = 0;
    tile_done_[i] = true;
  }
}

ArrayReadState::~ArrayReadState() { 
  if(max_overlap_range_ != NULL)
    free(max_overlap_range_);
 
  if(range_global_tile_coords_ != NULL)
    free(range_global_tile_coords_);

  if(range_global_tile_domain_ != NULL)
    free(range_global_tile_domain_);
}

/* ****************************** */
/*          READ FUNCTIONS        */
/* ****************************** */

int ArrayReadState::read_multiple_fragments(
    void** buffers, 
    size_t* buffer_sizes) {
  // Sanity check
  assert(array_->fragment_num());

  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int attribute_num = array_schema->attribute_num();
  std::vector<Fragment*> fragments = array_->fragments();
  int fragment_num = fragments.size();

  // Reset overflow
  overflow_.resize(attribute_num+1); 
  for(int i=0; i<attribute_num+1; ++i)
    overflow_[i] = false;
  done_ = false;
 
  for(int i=0; i<fragment_num; ++i)
    fragments[i]->reset_overflow();

  if(array_schema->dense())  // DENSE case
    return read_multiple_fragments_dense(buffers, buffer_sizes);
  else                       // SPARSE case
    return read_multiple_fragments_sparse(buffers, buffer_sizes);
}

/* ****************************** */
/*         PRIVATE METHODS        */
/* ****************************** */

void ArrayReadState::clean_up_processed_fragment_cell_pos_ranges() {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int attribute_num = array_schema->attribute_num();

  // Find the minimum overlapping tile position across all attributes
  const std::vector<int>& attribute_ids = array_->attribute_ids();
  int attribute_id_num = attribute_ids.size(); 
  int min_pos = fragment_cell_pos_ranges_vec_pos_[0];
  for(int i=1; i<attribute_id_num; ++i) 
    if(fragment_cell_pos_ranges_vec_pos_[attribute_ids[i]] < min_pos) 
      min_pos = fragment_cell_pos_ranges_vec_pos_[attribute_ids[i]];

  // Clean up processed overlapping tiles
  if(min_pos != 0) {
    // Remove overlapping tile elements from the vector
    FragmentCellPosRangesVec::iterator it_first = 
         fragment_cell_pos_ranges_vec_.begin(); 
    FragmentCellPosRangesVec::iterator it_last = it_first + min_pos; 
    fragment_cell_pos_ranges_vec_.erase(it_first, it_last); 

    // Update the positions
    for(int i=0; i<attribute_num+1; ++i)
      if(fragment_cell_pos_ranges_vec_pos_[i] != 0)
        fragment_cell_pos_ranges_vec_pos_[i] -= min_pos;
  }
}

template<class T>
void ArrayReadState::copy_cell_range_with_empty(
    int attribute_id,
    void* buffer,  
    size_t buffer_size,
    size_t& buffer_offset,
    const CellPosRange& cell_pos_range) {
// TODO
}

template<class T>
int ArrayReadState::copy_cell_ranges(
    int attribute_id,
    void* buffer,  
    size_t buffer_size,
    size_t& buffer_offset) {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int64_t pos = fragment_cell_pos_ranges_vec_pos_[attribute_id];
  size_t coords_size = array_schema->coords_size();
  FragmentCellPosRanges& fragment_cell_pos_ranges = 
      fragment_cell_pos_ranges_vec_[pos];
  int64_t fragment_cell_pos_ranges_num = fragment_cell_pos_ranges.size();
  std::vector<Fragment*> fragments = array_->fragments();
  int fragment_num = fragments.size();
  int fragment_i;

  // Sanity check
  assert(!array_schema->var_size(attribute_id));

  // Copy the cell ranges one by one
  for(int64_t i=0; i<fragment_cell_pos_ranges_num; ++i) {
    fragment_i = fragment_cell_pos_ranges[i].first; 
    CellPosRange& cell_pos_range = fragment_cell_pos_ranges[i].second; 

    // Handle empty fragment
    if(fragment_i == -1) {
      copy_cell_range_with_empty<T>(
           attribute_id,
           buffer,
           buffer_size,
           buffer_offset,
           cell_pos_range);
      if(overflow_[attribute_id])
        break;
      else
        continue;
    }

    // Handle non-empty fragment
    if(fragments[fragment_i]->copy_cell_range<T>(
           attribute_id,
           buffer,
           buffer_size,
           buffer_offset,
           cell_pos_range) != TILEDB_FG_OK)
       return TILEDB_ARS_ERR;

     // Handle overflow
     if(fragments[fragment_i]->overflow(attribute_id)) {
       overflow_[attribute_id] = true;
       break;
     }
  } 

  // Handle the case the tile is done for this attribute
  if(!overflow_[attribute_id]) {
    for(int i=0; i<fragment_num; ++i)
      if(fragment_global_tile_coords_[i] != NULL &&
         !memcmp(
              fragment_global_tile_coords_[i], 
              range_global_tile_coords_, 
              coords_size))
        fragments[i]->tile_done(attribute_id);
    ++fragment_cell_pos_ranges_vec_pos_[attribute_id];
    tile_done_[attribute_id] = true;
  } else {
    tile_done_[attribute_id] = false;
  }

  // Success
  return TILEDB_ARS_OK;
}

template<class T>
int ArrayReadState::compute_fragment_cell_pos_ranges(
    const FragmentCellRanges& unsorted_fragment_cell_ranges,
    FragmentCellPosRanges& fragment_cell_pos_ranges) const {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int dim_num = array_schema->dim_num();
  size_t coords_size = array_schema->coords_size();
  const T* global_domain = static_cast<const T*>(array_schema->domain());
  const T* tile_extents = static_cast<const T*>(array_schema->tile_extents());
  const T* tile_coords = static_cast<const T*>(range_global_tile_coords_);
  std::vector<Fragment*> fragments = array_->fragments();

  // Populate queue
  std::priority_queue<
      FragmentCellRange,
      FragmentCellRanges,
      SmallerFragmentCellRange<T> > pq(array_schema);
  int unsorted_fragment_cell_ranges_num = unsorted_fragment_cell_ranges.size();
  for(int64_t i=0; i<unsorted_fragment_cell_ranges_num; ++i) 
    pq.push(unsorted_fragment_cell_ranges[i]);

  // Compute tile domain
  T* tile_domain = new T[2*dim_num];
  T* tile_domain_end = new T[dim_num];
  for(int i=0; i<dim_num; ++i) {
    tile_domain[2*i] = global_domain[2*i] + tile_coords[i] * tile_extents[i];
    tile_domain[2*i+1] = tile_domain[2*i] + tile_extents[i] - 1;
    tile_domain_end[i] = tile_domain[2*i+1];
  }
  
  // Start processing the queue
  FragmentCellRanges fragment_cell_ranges;
  FragmentCellRange popped, top;
  int popped_fragment_i, top_fragment_i;
  T* popped_range, *top_range;
  while(!pq.empty()) {
    // Pop the first entry and mark it as popped
    popped = pq.top();
    popped_fragment_i = popped.first;
    popped_range = static_cast<T*>(popped.second);
    pq.pop();

    // Trivial case: just insert the popped range into the results and stop
    if(pq.empty()) {
      if(fragments[popped_fragment_i]->dense() ||
         memcmp(
               popped_range, 
               &popped_range[dim_num], 
               coords_size) || 
          fragments[popped_fragment_i]->coords_exist<T>(popped_range)) {
        fragment_cell_ranges.push_back(popped);
        break;
      }
    }

    // Mark the second entry (now top) as top
    top = pq.top();
    top_fragment_i = top.first;
    top_range = static_cast<T*>(top.second);

    // Dinstinguish two cases
    if(fragments[popped_fragment_i]->dense() ||
       !memcmp(
           popped_range, 
           &popped_range[dim_num], 
           coords_size)) {                          // DENSE POPPED OR UNARY
      // If the unary sparse range is empty, discard it
      if(!fragments[popped_fragment_i]->dense() && 
         !fragments[popped_fragment_i]->coords_exist<T>(popped_range)) {
        free(popped.second);
        continue;
      }

      // Keep on discarding ranges from the queue
      while(!pq.empty() &&
            top_fragment_i < popped_fragment_i &&
            array_schema->cell_order_cmp(
                top_range, 
                popped_range) >= 0 &&
            array_schema->cell_order_cmp(
                top_range, 
                &popped_range[dim_num]) <= 0) {

        // Cut the top range and re-insert, only if there is partial overlap
        if(array_schema->cell_order_cmp(
               &top_range[dim_num], 
               &popped_range[dim_num]) > 0) {
          // Create the new range
          FragmentCellRange trimmed_top;
          trimmed_top.first = top_fragment_i;
          trimmed_top.second = malloc(2*coords_size);
          T* trimmed_top_range = static_cast<T*>(trimmed_top.second);
          memcpy(
              trimmed_top_range, 
              &popped_range[dim_num], 
              coords_size);
          memcpy(
              &trimmed_top_range[dim_num], 
              &top_range[dim_num], 
              coords_size);

          // Advance the first trimmed range coordinates by one
          array_schema->get_next_cell_coords<T>(tile_domain, trimmed_top_range);

          // Re-insert into the queue
          pq.push(trimmed_top);
        } else { // Simply discard top and get a new one
          free(top.second);
        }

        // Get a new top
        pq.pop();
        top = pq.top();
        top_fragment_i = top.first;
        top_range = static_cast<T*>(top.second);
      }

      //Potentially trim the popped range
      if(!pq.empty() && 
         top_fragment_i > popped_fragment_i && 
         array_schema->cell_order_cmp(
             top_range, 
             &popped_range[dim_num]) <= 0) {
         // Potentially create a new popped range
         if(array_schema->cell_order_cmp(
                &top_range[dim_num], 
                &popped_range[dim_num]) < 0) {
           FragmentCellRange extra_popped;
           extra_popped.first = popped_fragment_i;
           extra_popped.second = malloc(2*coords_size);
           T* extra_popped_range = static_cast<T*>(extra_popped.second);
           memcpy(extra_popped_range, &top_range[dim_num], coords_size);
           memcpy(
               &extra_popped_range[dim_num], 
               &popped_range[dim_num], 
               coords_size);
           array_schema->get_next_cell_coords<T>(
               tile_domain, 
               extra_popped_range);

           // Re-instert the extra popped range into the queue
           pq.push(extra_popped);
         }

         // Trim last range coordinates of popped
         memcpy(&popped_range[dim_num], top_range, coords_size);

         // Get previous cell of the last range coordinates of popped
         array_schema->get_previous_cell_coords<T>(
             tile_domain, 
             &popped_range[dim_num]);
       }  
     
       // Insert the final popped range into the results
       fragment_cell_ranges.push_back(popped);
    } else {                               // SPARSE POPPED
      // If popped does not overlap with top, insert popped into results
      if(!pq.empty() &&
         array_schema->cell_order_cmp(
             top_range, 
             &popped_range[dim_num]) > 0) {
        fragment_cell_ranges.push_back(popped);
      } else { // Need to expand popped
        // Create a new unary range
        FragmentCellRange unary;
        unary.first = popped_fragment_i;
        unary.second = malloc(2*coords_size);
        T* unary_range = static_cast<T*>(unary.second);
        
        // Get the first two coordinates from the coordinates tile 
        if(fragments[popped_fragment_i]->get_first_two_coords<T>(
               popped_range,   // Starting point
               unary_range,    // First coords
               popped_range)   // Second coords
            != TILEDB_FG_OK) {  
          // Clean up
          free(unary.second);
          delete [] tile_domain;
          delete [] tile_domain_end;
          free(popped.second);
          free(top.second);
          while(!pq.empty()) {
            free(pq.top().second);
            pq.pop();
          }
          for(int i=0; i<fragment_cell_ranges.size(); ++i)
            free(fragment_cell_ranges[i].second);

          return TILEDB_ARS_ERR;
        }

        // Copy second boundary of unary and re-insert to the queue
        if(array_schema->cell_order_cmp(
               unary_range,
               tile_domain_end) > 0) {
          free(unary.second);
        } else {
          memcpy(&unary_range[dim_num], unary_range, coords_size);
          pq.push(unary);

          // Check if the now trimmed popped range exceeds the tile domain
          if(array_schema->cell_order_cmp(
                 popped_range,
                 tile_domain_end) > 0) {
            free(popped.second);
          } else {
            // Re-insert to the queue the now trimmed popped range
            pq.push(popped);
          }
        }
      }
    }
  }

  // Sanity check
  assert(pq.empty());

  // Compute fragment cell position ranges
  for(int64_t i=0; i<fragment_cell_ranges.size(); ++i) { 
    // Dense case
    if(fragment_cell_ranges[i].first == -1 ||
       fragments[fragment_cell_ranges[i].first]->dense()) {
      // Create a new fragment cell position range
      FragmentCellPosRange fragment_cell_pos_range;
      fragment_cell_pos_range.first = fragment_cell_ranges[i].first;
      CellPosRange& cell_pos_range = fragment_cell_pos_range.second;
      T* cell_range = static_cast<T*>(fragment_cell_ranges[i].second);

      // Normalize range
      for(int i=0; i<dim_num; ++i) { 
        cell_range[i] -= tile_domain[2*i];
        cell_range[dim_num+i] -= tile_domain[2*i];
      }
      cell_pos_range.first = array_schema->get_cell_pos(cell_range);
      cell_pos_range.second = array_schema->get_cell_pos(&cell_range[dim_num]);
      fragment_cell_pos_ranges.push_back(fragment_cell_pos_range); 
    } else { // Sparse case
     FragmentCellPosRanges sparse_cell_pos_ranges; 
     if(fragments[fragment_cell_ranges[i].first]->
            get_cell_pos_ranges_sparse<T>(
                fragment_cell_ranges[i].first,
                tile_domain,
                static_cast<T*>(fragment_cell_ranges[i].second),
                sparse_cell_pos_ranges) != TILEDB_FG_OK) {
        // Clean up
        for(int j=i; j < fragment_cell_ranges.size(); ++j) 
          free(fragment_cell_ranges[j].second);
        // Exit
        return TILEDB_ARS_ERR;
      }
      fragment_cell_pos_ranges.insert(
          fragment_cell_pos_ranges.end(),
          sparse_cell_pos_ranges.begin(), 
          sparse_cell_pos_ranges.end()); 
    }

    free(fragment_cell_ranges[i].second);
  }

  // Clean up 
  delete [] tile_domain;
  delete [] tile_domain_end;
  
  // Success
  return TILEDB_ARS_OK;
}

template<class T>
int ArrayReadState::get_next_cell_ranges_dense() {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int dim_num = array_schema->dim_num();
  size_t coords_size = array_schema->coords_size();
  std::vector<Fragment*> fragments = array_->fragments();
  int fragment_num = fragments.size();

  // First invocation: bring the first overlapping tile for each fragment
  if(fragment_cell_pos_ranges_vec_.size() == 0) {
    // Allocate space for the maximum overlap range
    max_overlap_range_ = malloc(2*coords_size);

    // Initialize range global tile coordinates
    init_range_global_tile_coords<T>();

    // Return if there are no more overlapping tiles
    if(range_global_tile_coords_ == NULL) {
      done_ = true;
      return TILEDB_ARS_OK;
    }

    // Get next overlapping tile and calculate its global position for
    // every fragment
    fragment_global_tile_coords_.resize(fragment_num);
    for(int i=0; i<fragment_num; ++i) { 
      fragments[i]->get_next_overlapping_tile_mult();
      fragment_global_tile_coords_[i] = 
          fragments[i]->get_global_tile_coords();
    }
  } else { 
    // Temporarily store the current range global tile coordinates
    assert(range_global_tile_coords_ != NULL);
    T* previous_range_global_tile_coords = new T[dim_num];
    memcpy(
        previous_range_global_tile_coords,
        range_global_tile_coords_,
        coords_size);

    // Advance range coordinates
    get_next_range_global_tile_coords<T>();

    // Return if there are no more overlapping tiles
    if(range_global_tile_coords_ == NULL) {
      done_ = true;
      delete [] previous_range_global_tile_coords;
      return TILEDB_ARS_OK;
    }

    // Subsequent invocations: get next overallping tiles for the processed
    // fragments
    for(int i=0; i<fragment_num; ++i) {
      if(fragment_global_tile_coords_[i] != NULL &&
         !memcmp(
             fragment_global_tile_coords_[i], 
             previous_range_global_tile_coords, 
             coords_size)) { 
        fragments[i]->get_next_overlapping_tile_mult();
        fragment_global_tile_coords_[i] = 
            fragments[i]->get_global_tile_coords(); 
      }
    }

    // Clean up
    delete [] previous_range_global_tile_coords;
  }

  // Advance properly the sparse fragments
  for(int i=0; i<fragment_num; ++i) {
    while(!fragments[i]->dense() && 
          fragment_global_tile_coords_[i] != NULL &&
          array_schema->tile_order_cmp<T>(
               static_cast<const T*>(fragment_global_tile_coords_[i]), 
               static_cast<const T*>(range_global_tile_coords_)) < 0) {
       fragments[i]->get_next_overlapping_tile_mult();
       fragment_global_tile_coords_[i] = 
           fragments[i]->get_global_tile_coords();
    }

  }

  // Compute the maximum overlap range for this tile
  compute_max_overlap_range<T>();

  // Find the most recent fragment with a full dense tile in the smallest global
  // tile position
  max_overlap_i_ = -1;
  for(int i=fragment_num-1; i>=0; --i) { 
    if(fragment_global_tile_coords_[i] != NULL &&
       !memcmp(
            fragment_global_tile_coords_[i], 
            range_global_tile_coords_, 
            coords_size) &&
       fragments[i]->max_overlap<T>(
           static_cast<const T*>(max_overlap_range_))) {
      max_overlap_i_ = i;
      break; 
    }
  } 

  // This will hold the unsorted fragment cell ranges
  FragmentCellRanges unsorted_fragment_cell_ranges;

  // Compute initial cell ranges for the fragment with the max overlap
  compute_max_overlap_fragment_cell_ranges<T>(unsorted_fragment_cell_ranges);  

  // Compute cell ranges for the rest of the relevant fragments
  for(int i=max_overlap_i_+1; i<fragment_num; ++i) {
    if(fragment_global_tile_coords_[i] != NULL &&
       !memcmp(
           fragment_global_tile_coords_[i], 
           range_global_tile_coords_, 
           coords_size)) { 
      fragments[i]->compute_fragment_cell_ranges<T>(
          i,
          unsorted_fragment_cell_ranges);
    }
  } 

  // Compute the fragment cell position ranges
  FragmentCellPosRanges fragment_cell_pos_ranges;
  if(compute_fragment_cell_pos_ranges<T>(
         unsorted_fragment_cell_ranges, 
         fragment_cell_pos_ranges) != TILEDB_ARS_OK) {
    // Clean up
    for(int64_t i=0; i<unsorted_fragment_cell_ranges.size(); ++i)
      free(unsorted_fragment_cell_ranges[i].second);
    return TILEDB_ARS_ERR;
  }

  // Insert cell pos ranges in the state
  fragment_cell_pos_ranges_vec_.push_back(fragment_cell_pos_ranges);

  // Clean up processed overlapping tiles
  clean_up_processed_fragment_cell_pos_ranges();

  // Success
  return TILEDB_ARS_OK;
}

template<class T>
void ArrayReadState::compute_max_overlap_fragment_cell_ranges(
    FragmentCellRanges& fragment_cell_ranges) const {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int dim_num = array_schema->dim_num();
  size_t coords_size = array_schema->coords_size();
  ArraySchema::CellOrder cell_order = array_schema->cell_order();
  size_t cell_range_size = 2*coords_size;
  const T* tile_extents = static_cast<const T*>(array_schema->tile_extents());
  const T* global_domain = static_cast<const T*>(array_schema->domain());
  const T* range_global_tile_coords = 
      static_cast<const T*>(range_global_tile_coords_);
  const T* max_overlap_range = 
      static_cast<const T*>(max_overlap_range_);

  // Compute global coordinates of max_overlap_range
  T* global_max_overlap_range = new T[2*dim_num]; 
  for(int i=0; i<dim_num; ++i) {
    global_max_overlap_range[2*i] = 
        range_global_tile_coords[i] * tile_extents[i] +
        max_overlap_range[2*i] + global_domain[2*i];
    global_max_overlap_range[2*i+1] = 
        range_global_tile_coords[i] * tile_extents[i] +
        max_overlap_range[2*i+1] + global_domain[2*i];
  }

  // Contiguous cells, single cell range
  if(max_overlap_type_ == FULL || max_overlap_type_ == PARTIAL_CONTIG) {
    void* cell_range = malloc(cell_range_size);
    T* cell_range_T = static_cast<T*>(cell_range);
    for(int i=0; i<dim_num; ++i) {
      cell_range_T[i] = global_max_overlap_range[2*i];
      cell_range_T[dim_num + i] = global_max_overlap_range[2*i+1];
    }
    fragment_cell_ranges.push_back(
        FragmentCellRange(max_overlap_i_, cell_range));
  } else { // Non-contiguous cells, multiple ranges
    // Initialize the coordinates at the beginning of the global range
    T* coords = new T[dim_num];
    for(int i=0; i<dim_num; ++i)
      coords[i] = global_max_overlap_range[2*i];

    // Handle the different cell orders
    int i;
    if(cell_order == ArraySchema::TILEDB_AS_CO_ROW_MAJOR) {           // ROW
      while(coords[0] <= global_max_overlap_range[1]) {
        // Make a cell range representing a slab       
        void* cell_range = malloc(cell_range_size);
        T* cell_range_T = static_cast<T*>(cell_range);
        for(int i=0; i<dim_num-1; ++i) { 
          cell_range_T[i] = coords[i];
          cell_range_T[dim_num+i] = coords[i];
        }
        cell_range_T[dim_num-1] = global_max_overlap_range[2*(dim_num-1)];
        cell_range_T[2*dim_num-1] = global_max_overlap_range[2*(dim_num-1)+1];

        // Insert the new range into the result vector
        fragment_cell_ranges.push_back(
            FragmentCellRange(max_overlap_i_, cell_range));
 
        // Advance coordinates
        i=dim_num-2;
        ++coords[i];
        while(i > 0 && coords[i] > global_max_overlap_range[2*i+1]) {
          coords[i] = global_max_overlap_range[2*i];
          ++coords[--i];
        } 
      }
    } else if(cell_order == ArraySchema::TILEDB_AS_CO_COLUMN_MAJOR) { // COLUMN
      while(coords[dim_num-1] <= global_max_overlap_range[2*(dim_num-1)+1]) {
        // Make a cell range representing a slab       
        void* cell_range = malloc(cell_range_size);
        T* cell_range_T = static_cast<T*>(cell_range);
        for(int i=dim_num-1; i>0; --i) { 
          cell_range_T[i] = coords[i];
          cell_range_T[dim_num+i] = coords[i];
        }
        cell_range_T[0] = global_max_overlap_range[0];
        cell_range_T[dim_num] = global_max_overlap_range[1];

        // Insert the new range into the result vector
        fragment_cell_ranges.push_back(
            FragmentCellRange(max_overlap_i_, cell_range));
 
        // Advance coordinates
        i=1;
        ++coords[i];
        while(i <dim_num-1 && coords[i] > global_max_overlap_range[2*i+1]) {
          coords[i] = global_max_overlap_range[2*i];
          ++coords[++i];
        } 
      }
    } else {
      assert(0);
    }

    delete [] coords;
  }

  // Clean up
  delete [] global_max_overlap_range;
}

template<class T>
void ArrayReadState::compute_max_overlap_range() {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int dim_num = array_schema->dim_num();
  ArraySchema::CellOrder cell_order = array_schema->cell_order();
  const T* tile_extents = static_cast<const T*>(array_schema->tile_extents());
  const T* global_domain = static_cast<const T*>(array_schema->domain());
  const T* range = static_cast<const T*>(array_->range());

  T tile_coords;
  T* max_overlap_range = static_cast<T*>(max_overlap_range_);
  const T* range_global_tile_coords = 
      static_cast<const T*>(range_global_tile_coords_);
  for(int i=0; i<dim_num; ++i) {
    tile_coords = 
        range_global_tile_coords[i] * tile_extents[i] + global_domain[2*i];
    max_overlap_range[2*i] = std::max(range[2*i] - tile_coords, T(0));
    max_overlap_range[2*i+1] = 
        std::min(range[2*i+1] - tile_coords, tile_extents[i] - 1);
  }

  // Check overlap
  max_overlap_type_ = FULL;
  for(int i=0; i<dim_num; ++i) {
    if(max_overlap_range[2*i] != 0 ||
       max_overlap_range[2*i+1] != tile_extents[i] - 1) {
      max_overlap_type_ = PARTIAL_NON_CONTIG;
      break;
    }
  }

  if(max_overlap_type_ == PARTIAL_NON_CONTIG) {
    max_overlap_type_ = PARTIAL_CONTIG;
    if(cell_order == ArraySchema::TILEDB_AS_CO_ROW_MAJOR) {   // Row major
      for(int i=1; i<dim_num; ++i) {
        if(max_overlap_range[2*i] != 0 ||
           max_overlap_range[2*i+1] != tile_extents[i] - 1) {
          max_overlap_type_ = PARTIAL_NON_CONTIG;
          break;
        }
      }
    } else if(cell_order == ArraySchema::TILEDB_AS_CO_COLUMN_MAJOR) { 
      // Column major
      for(int i=dim_num-2; i>=0; --i) {
        if(max_overlap_range[2*i] != 0 ||
           max_overlap_range[2*i+1] != tile_extents[i] - 1) {
          max_overlap_type_ = PARTIAL_NON_CONTIG;
          break;
        }
      }
    }
  }
}

template<class T>
void ArrayReadState::get_next_range_global_tile_coords() {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int dim_num = array_schema->dim_num();
  T* range_global_tile_domain = static_cast<T*>(range_global_tile_domain_);
  T* range_global_tile_coords = static_cast<T*>(range_global_tile_coords_);

  // Advance range tile coordinates
  array_schema->get_next_tile_coords<T>(
      range_global_tile_domain, 
      range_global_tile_coords);

  // Check if the new range coordinates fall out of the range domain
  bool inside_domain = true;
  for(int i=0; i<dim_num; ++i) {
    if(range_global_tile_coords[i] < range_global_tile_domain[2*i] ||
       range_global_tile_coords[i] > range_global_tile_domain[2*i+1]) {
      inside_domain = false;
      break;
    }
  }

  // The coordinates fall outside the domain
  if(!inside_domain) {  
    free(range_global_tile_domain_);
    range_global_tile_domain_ = NULL; 
    free(range_global_tile_coords_);
    range_global_tile_coords_ = NULL; 
  } 
}

template<class T>
void ArrayReadState::init_range_global_tile_coords() {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  int dim_num = array_schema->dim_num();
  size_t coords_size = array_schema->coords_size();
  const T* domain = static_cast<const T*>(array_schema->domain());
  const T* tile_extents = static_cast<const T*>(array_schema->tile_extents());
  const T* range = static_cast<const T*>(array_->range());

  // Sanity check
  assert(tile_extents != NULL);

  // Compute tile domain
  T* tile_domain = new T[2*dim_num];
  T tile_num; // Per dimension
  for(int i=0; i<dim_num; ++i) {
    tile_num = ceil(double(domain[2*i+1] - domain[2*i] + 1) / tile_extents[i]);
    tile_domain[2*i] = 0;
    tile_domain[2*i+1] = tile_num - 1;
  }

  // Allocate space for the range in tile domain
  assert(range_global_tile_domain_ == NULL);
  range_global_tile_domain_ = malloc(2*dim_num*sizeof(T));

  // For easy reference
  T* range_global_tile_domain = static_cast<T*>(range_global_tile_domain_);

  // Calculate range in tile domain
  for(int i=0; i<dim_num; ++i) {
    range_global_tile_domain[2*i] = 
        std::max((range[2*i] - domain[2*i]) / tile_extents[i],
            tile_domain[2*i]); 
    range_global_tile_domain[2*i+1] = 
        std::min((range[2*i+1] - domain[2*i]) / tile_extents[i],
            tile_domain[2*i+1]); 
  }

  // Check if there is any overlap between the range and the tile domain
  bool overlap = true;
  for(int i=0; i<dim_num; ++i) {
    if(range_global_tile_domain[2*i] > tile_domain[2*i+1] ||
       range_global_tile_domain[2*i+1] < tile_domain[2*i]) {
      overlap = false;
      break;
    }
  }

  // Calculate range global tile coordinates
  if(!overlap) {  // No overlap
    free(range_global_tile_domain_);
    range_global_tile_domain_ = NULL; 
  } else {        // Overlap
    range_global_tile_coords_ = malloc(coords_size);
    T* range_global_tile_coords = static_cast<T*>(range_global_tile_coords_);
    for(int i=0; i<dim_num; ++i) 
      range_global_tile_coords[i] = range_global_tile_domain[2*i]; 
  }

  // Clean up
  delete [] tile_domain;
}

int ArrayReadState::read_multiple_fragments_dense(
    void** buffers,  
    size_t* buffer_sizes) {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  std::vector<int> attribute_ids = array_->attribute_ids();
  int attribute_id_num = attribute_ids.size(); 

  // Write each attribute individually
  int buffer_i = 0;
  int rc;
  for(int i=0; i<attribute_id_num; ++i) {
    if(!array_schema->var_size(attribute_ids[i])) { // FIXED CELLS
      rc = read_multiple_fragments_dense_attr(
               attribute_ids[i], 
               buffers[buffer_i], 
               buffer_sizes[buffer_i]);

      if(rc != TILEDB_AR_OK)
        break;
      ++buffer_i;
    } else {                                         // VARIABLE-SIZED CELLS
      rc = read_multiple_fragments_dense_attr_var(
               attribute_ids[i], 
               buffers[buffer_i],       // offsets 
               buffer_sizes[buffer_i],
               buffers[buffer_i+1],     // actual values
               buffer_sizes[buffer_i+1]);

      if(rc != TILEDB_AR_OK)
        break;
      buffer_i += 2;
    }
  }

  return rc;
}

int ArrayReadState::read_multiple_fragments_dense_attr(
    int attribute_id,
    void* buffer,  
    size_t& buffer_size) {
  // For easy reference
  const ArraySchema* array_schema = array_->array_schema();
  const std::type_info* coords_type = array_schema->coords_type();

  // Invoke the proper templated function
  if(coords_type == &typeid(int)) {
    return read_multiple_fragments_dense_attr<int>(
               attribute_id, 
               buffer, 
               buffer_size);
  } else if(coords_type == &typeid(int64_t)) {
    return read_multiple_fragments_dense_attr<int64_t>(
               attribute_id, 
               buffer, 
               buffer_size);
  } else {
    PRINT_ERROR("Cannot read from array; Invalid coordinates type");
    return TILEDB_ARS_ERR;
  }
}

template<class T>
int ArrayReadState::read_multiple_fragments_dense_attr(
    int attribute_id,
    void* buffer,  
    size_t& buffer_size) {
  // Auxiliary variables
  size_t buffer_offset = 0;

  // Until read is done or there is a buffer overflow 
  for(;;) {
    // Continue copying from the previous unfinished tile
    if(!tile_done_[attribute_id])
      if(copy_cell_ranges<T>(
             attribute_id,
             buffer, 
             buffer_size, 
             buffer_offset) != TILEDB_ARS_OK)
        return TILEDB_ARS_ERR;

    // Check for overflow
    if(overflow_[attribute_id]) {
      buffer_size = buffer_offset;
      return TILEDB_ARS_OK;
    }

    // Prepare the cell ranges for the next tile under investigation, if
    // has not be done already
    if(fragment_cell_pos_ranges_vec_pos_[attribute_id] >= 
       fragment_cell_pos_ranges_vec_.size()) {
      // Get next overlapping tiles
      if(get_next_cell_ranges_dense<T>() != TILEDB_ARS_OK)
        return TILEDB_ARS_ERR;
    }

    // Check if read is done
    if(done_) {
      buffer_size = buffer_offset;
      return TILEDB_ARS_OK;
    }
 
    // Process the heap and copy cells to buffers
    if(copy_cell_ranges<T>(
           attribute_id, 
           buffer, 
           buffer_size, 
           buffer_offset) != TILEDB_ARS_OK)
      return TILEDB_ARS_ERR;

    // Check for buffer overflow
    if(overflow_[attribute_id]) {
      buffer_size = buffer_offset;
      return TILEDB_ARS_OK;
    }
  } 
}

int ArrayReadState::read_multiple_fragments_dense_attr_var(
    int attribute_id,
    void* buffer,  
    size_t& buffer_size,
    void* buffer_var,  
    size_t& buffer_var_size) {
  // TODO
}

int ArrayReadState::read_multiple_fragments_sparse(
    void** buffers,  
    size_t* buffer_sizes) {
  // TODO
}


template<class T>
ArrayReadState::SmallerFragmentCellRange<T>::SmallerFragmentCellRange()
    : array_schema_(NULL) { 
}

template<class T>
ArrayReadState::SmallerFragmentCellRange<T>::SmallerFragmentCellRange(
    const ArraySchema* array_schema)
    : array_schema_(array_schema) { 
}

template<class T>
bool ArrayReadState::SmallerFragmentCellRange<T>::operator () (
    FragmentCellRange a, 
    FragmentCellRange b) const {
  // Sanity check
  assert(array_schema_ != NULL);

  // Get cell ordering information for the first range endpoints
  int cmp = array_schema_->cell_order_cmp<T>(
      static_cast<const T*>(a.second), 
      static_cast<const T*>(b.second)); 

  if(cmp < 0)      // a's range start precedes b's
    return false;
  else if(cmp > 0) // b's range start preceded a's
    return true;
  else             // a's and b's range starts match - latest fragment wins
    return (a.first < b.first); 
}

// Explicit template instantiations
template class ArrayReadState::SmallerFragmentCellRange<int>;
template class ArrayReadState::SmallerFragmentCellRange<int64_t>;
template class ArrayReadState::SmallerFragmentCellRange<float>;
template class ArrayReadState::SmallerFragmentCellRange<double>;
