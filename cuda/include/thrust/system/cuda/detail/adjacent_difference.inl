/*
 *  Copyright 2008-2013 NVIDIA Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <thrust/detail/config.h>

#include <thrust/adjacent_difference.h>
#include <thrust/gather.h>
#include <thrust/functional.h>
#include <thrust/iterator/iterator_traits.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/iterator/transform_iterator.h>
#include <thrust/detail/temporary_array.h>
#include <thrust/system/detail/internal/decompose.h>
#include <thrust/system/cuda/detail/default_decomposition.h>
#include <thrust/system/cuda/detail/detail/launch_closure.h>
#include <thrust/system/cuda/detail/detail/launch_calculator.h>
#include <thrust/system/cuda/detail/execution_policy.h>
#include <thrust/detail/seq.h>

namespace thrust
{
namespace system
{
namespace cuda
{
namespace detail
{
namespace adjacent_difference_detail
{


template<typename Decomposition>
struct last_index_in_each_interval : public thrust::unary_function<typename Decomposition::index_type, typename Decomposition::index_type>
{
  typedef typename Decomposition::index_type index_type;

  Decomposition decomp;

  __host__ __device__
  last_index_in_each_interval(Decomposition decomp) : decomp(decomp) {}

  __host__ __device__
  index_type operator()(index_type interval)
  {
    return decomp[interval].end() - 1;
  }
};


template <typename InputIterator1,
          typename InputIterator2,
          typename OutputIterator,
          typename BinaryFunction,
          typename Decomposition,
          typename Context>
struct adjacent_difference_closure
{
  InputIterator1 input;
  InputIterator2 input_copy;
  OutputIterator output;
  BinaryFunction binary_op;
  Decomposition  decomp;
  Context        context;

  typedef Context context_type;
  
  __host__ __device__
  adjacent_difference_closure(InputIterator1 input,
                              InputIterator2 input_copy,
                              OutputIterator output,
                              BinaryFunction binary_op,
                              Decomposition  decomp,
                              Context        context = Context())
    : input(input), input_copy(input_copy), output(output), binary_op(binary_op), decomp(decomp), context(context) {}

  __device__ __thrust_forceinline__
  void operator()(void)
  {
    typedef typename thrust::iterator_value<InputIterator1>::type  InputType;
    typedef typename Decomposition::index_type index_type;

    // this block processes results in [range.begin(), range.end())
    thrust::system::detail::internal::index_range<index_type> range = decomp[context.block_index()];
    
    input_copy += context.block_index() - 1;
      
    // prime the temp values for all threads so we don't need to launch a default constructor
    InputType next_left = (context.block_index() == 0) ? thrust::raw_reference_cast(*input) : thrust::raw_reference_cast(*input_copy);

    index_type base = range.begin();
    index_type i    = range.begin() + context.thread_index();
    
    if(i < range.end())
    {
      if(context.thread_index() > 0)
      {
        InputIterator1 temp = input + (i - 1);
        next_left = *temp;
      }              
    }
    
    input  += i;
    output += i;

    while(base < range.end())
    {
      InputType curr_left = next_left;

      if(i + context.block_dimension() < range.end())
      {
        InputIterator1 temp = input + (context.block_dimension() - 1);
        next_left = *temp;
      }

      context.barrier();

      if(i < range.end())
      {
        if(i == 0)
        {
          *output = *input;
        }
        else
        {
          InputType x = *input;
          *output = binary_op(x, curr_left);
        }
      }

      i      += context.block_dimension();
      base   += context.block_dimension();
      input  += context.block_dimension();
      output += context.block_dimension();
    }
  }
};


template<typename DerivedPolicy,
         typename InputIterator,
         typename OutputIterator,
         typename BinaryFunction>
__host__ __device__
OutputIterator adjacent_difference(execution_policy<DerivedPolicy> &exec,
                                   InputIterator first, InputIterator last,
                                   OutputIterator result,
                                   BinaryFunction binary_op)
{
  typedef typename thrust::iterator_value<InputIterator>::type                        InputType;
  typedef typename thrust::iterator_difference<InputIterator>::type                   IndexType;
  typedef          thrust::system::detail::internal::uniform_decomposition<IndexType> Decomposition;

  IndexType n = last - first;

  if(n == 0)
  {
    return result;
  }

  Decomposition decomp = default_decomposition(last - first);

  // allocate temporary storage
  thrust::detail::temporary_array<InputType,DerivedPolicy> temp(exec, decomp.size() - 1);

  // gather last value in each interval
  last_index_in_each_interval<Decomposition> unary_op(decomp);
  thrust::gather(exec,
                 thrust::make_transform_iterator(thrust::counting_iterator<IndexType>(0), unary_op),
                 thrust::make_transform_iterator(thrust::counting_iterator<IndexType>(0), unary_op) + (decomp.size() - 1),
                 first,
                 temp.begin());

  
  typedef typename thrust::detail::temporary_array<InputType,DerivedPolicy>::iterator InputIterator2;
  typedef detail::blocked_thread_array Context;
  typedef adjacent_difference_closure<InputIterator,InputIterator2,OutputIterator,BinaryFunction,Decomposition,Context> Closure;

  Closure closure(first, temp.begin(), result, binary_op, decomp); 

  detail::launch_closure(exec, closure, decomp.size());
  
  return result + n;
} // end adjacent_difference()


} // end namespace adjacent_difference_detail


__THRUST_DISABLE_MSVC_POSSIBLE_LOSS_OF_DATA_WARNING_BEGIN


template<typename DerivedPolicy,
         typename InputIterator,
         typename OutputIterator,
         typename BinaryFunction>
__host__ __device__
OutputIterator adjacent_difference(execution_policy<DerivedPolicy> &exec,
                                   InputIterator first, InputIterator last,
                                   OutputIterator result,
                                   BinaryFunction binary_op)
{
  // we're attempting to launch a kernel, assert we're compiling with nvcc
  // ========================================================================
  // X Note to the user: If you've found this line due to a compiler error, X
  // X you need to compile your code using nvcc, rather than g++ or cl.exe  X
  // ========================================================================
  THRUST_STATIC_ASSERT( (thrust::detail::depend_on_instantiation<InputIterator, THRUST_DEVICE_COMPILER == THRUST_DEVICE_COMPILER_NVCC>::value) );

  struct workaround
  {
    __host__ __device__
    static OutputIterator parallel_path(execution_policy<DerivedPolicy> &exec,
                                        InputIterator first, InputIterator last,
                                        OutputIterator result,
                                        BinaryFunction binary_op)
    {
      return thrust::system::cuda::detail::adjacent_difference_detail::adjacent_difference(exec, first, last, result, binary_op);
    }

    __host__ __device__
    static OutputIterator sequential_path(execution_policy<DerivedPolicy> &,
                                          InputIterator first, InputIterator last,
                                          OutputIterator result,
                                          BinaryFunction binary_op)
    {
      return thrust::adjacent_difference(thrust::seq, first, last, result, binary_op);
    }
  };

#if __BULK_HAS_CUDART__
  return workaround::parallel_path(exec, first, last, result, binary_op);
#else
  return workaround::sequential_path(exec, first, last, result, binary_op);
#endif
}


__THRUST_DISABLE_MSVC_POSSIBLE_LOSS_OF_DATA_WARNING_END


} // end namespace detail
} // end namespace cuda
} // end namespace system
} // end namespace thrust

