/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2020 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Definition of layer AffineTransform of NNUE evaluation function

#ifndef NNUE_LAYERS_AFFINE_TRANSFORM_H_INCLUDED
#define NNUE_LAYERS_AFFINE_TRANSFORM_H_INCLUDED

#include <iostream>
#include "../nnue_common.h"

namespace Eval::NNUE::Layers {

  // Affine transformation layer
  template <typename PreviousLayer, IndexType OutputDimensions>
  class AffineTransform {
   public:
    // Input/output type
    using InputType = typename PreviousLayer::OutputType;
    using OutputType = std::int32_t;
    static_assert(std::is_same<InputType, std::uint8_t>::value, "");

    // Number of input/output dimensions
    static constexpr IndexType kInputDimensions =
        PreviousLayer::kOutputDimensions;
    static constexpr IndexType kOutputDimensions = OutputDimensions;
    static constexpr IndexType kPaddedInputDimensions =
        CeilToMultiple<IndexType>(kInputDimensions, kMaxSimdWidth);

    // Size of forward propagation buffer used in this layer
    static constexpr std::size_t kSelfBufferSize =
        CeilToMultiple(kOutputDimensions * sizeof(OutputType), kCacheLineSize);

    // Size of the forward propagation buffer used from the input layer to this layer
    static constexpr std::size_t kBufferSize =
        PreviousLayer::kBufferSize + kSelfBufferSize;

    // Hash value embedded in the evaluation file
    static constexpr std::uint32_t GetHashValue() {
      std::uint32_t hash_value = 0xCC03DAE4u;
      hash_value += kOutputDimensions;
      hash_value ^= PreviousLayer::GetHashValue() >> 1;
      hash_value ^= PreviousLayer::GetHashValue() << 31;
      return hash_value;
    }

   // Read network parameters
    bool ReadParameters(std::istream& stream) {
      if (!previous_layer_.ReadParameters(stream)) return false;
      for (std::size_t i = 0; i < kOutputDimensions; ++i)
        biases_[i] = read_little_endian<BiasType>(stream);
      for (std::size_t i = 0; i < kOutputDimensions * kPaddedInputDimensions; ++i)
        weights_[i] = read_little_endian<WeightType>(stream);
      return !stream.fail();
    }

    // Forward propagation
    const OutputType* Propagate(
        const TransformedFeatureType* transformed_features, char* buffer) const {
      const auto input = previous_layer_.Propagate(
          transformed_features, buffer + kSelfBufferSize);

#if defined (USE_AVX512)

      [[maybe_unused]] const __m512i kOnes512 = _mm512_set1_epi16(1);

      [[maybe_unused]] auto m512_hadd = [](__m512i sum, int bias) -> int {
        return _mm512_reduce_add_epi32(sum) + bias;
      };

      [[maybe_unused]] auto m512_haddx4 = [](__m512i sum0, __m512i sum1, __m512i sum2, __m512i sum3, __m128i bias) -> __m128i {
        __m512i sum01a = _mm512_unpacklo_epi32(sum0, sum1);
        __m512i sum01b = _mm512_unpackhi_epi32(sum0, sum1);

        __m512i sum23a = _mm512_unpacklo_epi32(sum2, sum3);
        __m512i sum23b = _mm512_unpackhi_epi32(sum2, sum3);

        __m512i sum01 = _mm512_add_epi32(sum01a, sum01b);
        __m512i sum23 = _mm512_add_epi32(sum23a, sum23b);

        __m512i sum0123a = _mm512_unpacklo_epi64(sum01, sum23);
        __m512i sum0123b = _mm512_unpackhi_epi64(sum01, sum23);

        __m512i sum = _mm512_add_epi32(sum0123a, sum0123b);

        __m256i sum256lo = _mm512_castsi512_si256(sum);
        __m256i sum256hi = _mm512_extracti64x4_epi64(sum, 1);

        sum256lo = _mm256_add_epi32(sum256lo, sum256hi);

        __m128i sum128lo = _mm256_castsi256_si128(sum256lo);
        __m128i sum128hi = _mm256_extracti128_si256(sum256lo, 1);

        return _mm_add_epi32(_mm_add_epi32(sum128lo, sum128hi), bias);
      };

      [[maybe_unused]] auto m512_add_dpbusd_epi32 = [=](__m512i& acc, __m512i a, __m512i b) {
#if defined (USE_VNNI)
        acc = _mm512_dpbusd_epi32(acc, a, b);
#else
        __m512i product0 = _mm512_maddubs_epi16(a, b);
        product0 = _mm512_madd_epi16(product0, kOnes512);
        acc = _mm512_add_epi32(acc, product0);
#endif
      };

#endif
#if defined (USE_AVX2)

      [[maybe_unused]] const __m256i kOnes256 = _mm256_set1_epi16(1);

      [[maybe_unused]] auto m256_hadd = [](__m256i sum, int bias) -> int {
        __m128i sum128 = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
        sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_PERM_BADC));
        sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_PERM_CDAB));
        return _mm_cvtsi128_si32(sum128) + bias;
      };

      [[maybe_unused]] auto m256_haddx4 = [](__m256i sum0, __m256i sum1, __m256i sum2, __m256i sum3, __m128i bias) -> __m128i {
        sum0 = _mm256_hadd_epi32(sum0, sum1);
        sum2 = _mm256_hadd_epi32(sum2, sum3);

        sum0 = _mm256_hadd_epi32(sum0, sum2);

        __m128i sum128lo = _mm256_castsi256_si128(sum0);
        __m128i sum128hi = _mm256_extracti128_si256(sum0, 1);

        return _mm_add_epi32(_mm_add_epi32(sum128lo, sum128hi), bias);
      };

      [[maybe_unused]] auto m256_add_dpbusd_epi32 = [=](__m256i& acc, __m256i a, __m256i b) {
#if defined (USE_VNNI)
        acc = _mm256_dpbusd_epi32(acc, a, b);
#else
        __m256i product0 = _mm256_maddubs_epi16(a, b);
        product0 = _mm256_madd_epi16(product0, kOnes256);
        acc = _mm256_add_epi32(acc, product0);
#endif
      };

#endif

#if defined (USE_SSSE3)

      [[maybe_unused]] const __m128i kOnes128 = _mm_set1_epi16(1);

      [[maybe_unused]] auto m128_hadd = [](__m128i sum, int bias) -> int {
        sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0x4E)); //_MM_PERM_BADC
        sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0xB1)); //_MM_PERM_CDAB
        return _mm_cvtsi128_si32(sum) + bias;
      };

      [[maybe_unused]] auto m128_haddx4 = [](__m128i sum0, __m128i sum1, __m128i sum2, __m128i sum3, __m128i bias) -> __m128i {
        sum0 = _mm_hadd_epi32(sum0, sum1);
        sum2 = _mm_hadd_epi32(sum2, sum3);

        sum0 = _mm_hadd_epi32(sum0, sum2);

        return _mm_add_epi32(sum0, bias);
      };

      [[maybe_unused]] auto m128_add_dpbusd_epi32 = [=](__m128i& acc, __m128i a, __m128i b) {
        __m128i product0 = _mm_maddubs_epi16(a, b);
        product0 = _mm_madd_epi16(product0, kOnes128);
        acc = _mm_add_epi32(acc, product0);
      };

#endif

#if defined (USE_AVX512)

      constexpr IndexType kNumChunks512 = kPaddedInputDimensions / (kSimdWidth * 2);
      constexpr IndexType kNumChunks256 = kPaddedInputDimensions / kSimdWidth;

      const auto output = reinterpret_cast<OutputType*>(buffer);

      // Since to saturate a zmm register it takes 64 bytes we
      // cannot use AVX512 for the smaller affine transforms.
      // Instead we fallback to a AVX2 implementation if the
      // kInputDimensions isn't a multiple of 64.
      // Note that this means that for example for
      // kInputDimensions of 96 we fallback to AVX2 even though
      // the first 64 elements could be processed with AVX512.
      // This is caused by mixing the __m256 and __m512 variables
      // required to better handle that case and it would
      // require handling more cases statically not to lose performance.
      // This should be revisited if such input dimensions are to be considered.
      [[maybe_unused]] const auto input_vector512 = reinterpret_cast<const __m512i*>(input);
      [[maybe_unused]] const auto input_vector256 = reinterpret_cast<const __m256i*>(input);

      // kOutputDimensions is either 1 or a multiple of kSimdWidth
      // because then it is also an input dimension.
      if constexpr (kOutputDimensions % 4 == 0)
      {
        for (IndexType i = 0; i < kOutputDimensions; i += 4)
        {
          const IndexType offset0 = (i + 0) * kPaddedInputDimensions;
          const IndexType offset1 = (i + 1) * kPaddedInputDimensions;
          const IndexType offset2 = (i + 2) * kPaddedInputDimensions;
          const IndexType offset3 = (i + 3) * kPaddedInputDimensions;

          const __m128i bias = *reinterpret_cast<const __m128i*>(&biases_[i]);
          __m128i* outptr = reinterpret_cast<__m128i*>(&output[i]);

          if constexpr (kPaddedInputDimensions % (kSimdWidth * 2) == 0)
          {
            __m512i sum0 = _mm512_setzero_si512();
            __m512i sum1 = _mm512_setzero_si512();
            __m512i sum2 = _mm512_setzero_si512();
            __m512i sum3 = _mm512_setzero_si512();

            const auto row0 = reinterpret_cast<const __m512i*>(&weights_[offset0]);
            const auto row1 = reinterpret_cast<const __m512i*>(&weights_[offset1]);
            const auto row2 = reinterpret_cast<const __m512i*>(&weights_[offset2]);
            const auto row3 = reinterpret_cast<const __m512i*>(&weights_[offset3]);

            for (IndexType j = 0; j < kNumChunks512; ++j)
            {
              const __m512i in = input_vector512[j];

              m512_add_dpbusd_epi32(sum0, in, row0[j]);
              m512_add_dpbusd_epi32(sum1, in, row1[j]);
              m512_add_dpbusd_epi32(sum2, in, row2[j]);
              m512_add_dpbusd_epi32(sum3, in, row3[j]);
            }

            *outptr = m512_haddx4(sum0, sum1, sum2, sum3, bias);
          }
          else
          {
            __m256i sum0 = _mm256_setzero_si256();
            __m256i sum1 = _mm256_setzero_si256();
            __m256i sum2 = _mm256_setzero_si256();
            __m256i sum3 = _mm256_setzero_si256();

            const auto row0 = reinterpret_cast<const __m256i*>(&weights_[offset0]);
            const auto row1 = reinterpret_cast<const __m256i*>(&weights_[offset1]);
            const auto row2 = reinterpret_cast<const __m256i*>(&weights_[offset2]);
            const auto row3 = reinterpret_cast<const __m256i*>(&weights_[offset3]);

            for (IndexType j = 0; j < kNumChunks256; ++j)
            {
              const __m256i in = input_vector256[j];

              m256_add_dpbusd_epi32(sum0, in, row0[j]);
              m256_add_dpbusd_epi32(sum1, in, row1[j]);
              m256_add_dpbusd_epi32(sum2, in, row2[j]);
              m256_add_dpbusd_epi32(sum3, in, row3[j]);
            }

            *outptr = m256_haddx4(sum0, sum1, sum2, sum3, bias);
          }
        }
      }
      else if constexpr (kOutputDimensions == 1)
      {
        if constexpr (kPaddedInputDimensions % (kSimdWidth * 2) == 0)
        {
          __m512i sum0 = _mm512_setzero_si512();

          const auto row0 = reinterpret_cast<const __m512i*>(&weights_[0]);

          for (IndexType j = 0; j < kNumChunks512; ++j)
          {
            const __m512i in = input_vector512[j];

            m512_add_dpbusd_epi32(sum0, in, row0[j]);
          }

          output[0] = m512_hadd(sum0, biases_[0]);
        }
        else
        {
          __m256i sum0 = _mm256_setzero_si256();

          const auto row0 = reinterpret_cast<const __m256i*>(&weights_[0]);

          for (IndexType j = 0; j < kNumChunks256; ++j)
          {
            const __m256i in = input_vector256[j];

            m256_add_dpbusd_epi32(sum0, in, row0[j]);
          }

          output[0] = m256_hadd(sum0, biases_[0]);
        }
      }
      else
      {
        // This case can never happen because kOutputDimensions
        // is always 1 or a multiple of kSimdWidth.
        assert(false);
      }

#elif defined (USE_AVX2)

      constexpr IndexType kNumChunks = kPaddedInputDimensions / kSimdWidth;

      const auto output = reinterpret_cast<OutputType*>(buffer);
      const auto input_vector = reinterpret_cast<const __m256i*>(input);

      // kOutputDimensions is either 1 or a multiple of kSimdWidth
      // because then it is also an input dimension.
      if constexpr (kOutputDimensions % 4 == 0)
      {
        for (IndexType i = 0; i < kOutputDimensions; i += 4)
        {
          const IndexType offset0 = (i + 0) * kPaddedInputDimensions;
          const IndexType offset1 = (i + 1) * kPaddedInputDimensions;
          const IndexType offset2 = (i + 2) * kPaddedInputDimensions;
          const IndexType offset3 = (i + 3) * kPaddedInputDimensions;

          const __m128i bias = *reinterpret_cast<const __m128i*>(&biases_[i]);
          __m128i* outptr = reinterpret_cast<__m128i*>(&output[i]);

          __m256i sum0 = _mm256_setzero_si256();
          __m256i sum1 = _mm256_setzero_si256();
          __m256i sum2 = _mm256_setzero_si256();
          __m256i sum3 = _mm256_setzero_si256();

          const auto row0 = reinterpret_cast<const __m256i*>(&weights_[offset0]);
          const auto row1 = reinterpret_cast<const __m256i*>(&weights_[offset1]);
          const auto row2 = reinterpret_cast<const __m256i*>(&weights_[offset2]);
          const auto row3 = reinterpret_cast<const __m256i*>(&weights_[offset3]);

          for (IndexType j = 0; j < kNumChunks; ++j)
          {
            const __m256i in = input_vector[j];

            m256_add_dpbusd_epi32(sum0, in, row0[j]);
            m256_add_dpbusd_epi32(sum1, in, row1[j]);
            m256_add_dpbusd_epi32(sum2, in, row2[j]);
            m256_add_dpbusd_epi32(sum3, in, row3[j]);
          }

          *outptr = m256_haddx4(sum0, sum1, sum2, sum3, bias);
        }
      }
      else if constexpr (kOutputDimensions == 1)
      {
        __m256i sum0 = _mm256_setzero_si256();

        const auto row0 = reinterpret_cast<const __m256i*>(&weights_[0]);

        for (IndexType j = 0; j < kNumChunks; ++j)
        {
          const __m256i in = input_vector[j];

            m256_add_dpbusd_epi32(sum0, in, row0[j]);
        }

        output[0] = m256_hadd(sum0, biases_[0]);
      }
      else
      {
        // This case can never happen because kOutputDimensions
        // is always 1 or a multiple of kSimdWidth.
        assert(false);
      }

#elif defined (USE_SSSE3)

      constexpr IndexType kNumChunks = kPaddedInputDimensions / kSimdWidth;

      auto output = reinterpret_cast<OutputType*>(buffer);
      const auto input_vector = reinterpret_cast<const __m128i*>(input);

      // kOutputDimensions is either 1 or a multiple of kSimdWidth
      // because then it is also an input dimension.
      if constexpr (kOutputDimensions % 4 == 0)
      {
        for (IndexType i = 0; i < kOutputDimensions; i += 4)
        {
          const IndexType offset0 = (i + 0) * kPaddedInputDimensions;
          const IndexType offset1 = (i + 1) * kPaddedInputDimensions;
          const IndexType offset2 = (i + 2) * kPaddedInputDimensions;
          const IndexType offset3 = (i + 3) * kPaddedInputDimensions;

          const __m128i bias = *reinterpret_cast<const __m128i*>(&biases_[i]);
          __m128i* outptr = reinterpret_cast<__m128i*>(&output[i]);

          __m128i sum0 = _mm_setzero_si128();
          __m128i sum1 = _mm_setzero_si128();
          __m128i sum2 = _mm_setzero_si128();
          __m128i sum3 = _mm_setzero_si128();

          const auto row0 = reinterpret_cast<const __m128i*>(&weights_[offset0]);
          const auto row1 = reinterpret_cast<const __m128i*>(&weights_[offset1]);
          const auto row2 = reinterpret_cast<const __m128i*>(&weights_[offset2]);
          const auto row3 = reinterpret_cast<const __m128i*>(&weights_[offset3]);

          for (int j = 0; j < (int)kNumChunks; j += 1)
          {
            const __m128i in = input_vector[j];

            m128_add_dpbusd_epi32(sum0, in, row0[j]);
            m128_add_dpbusd_epi32(sum1, in, row1[j]);
            m128_add_dpbusd_epi32(sum2, in, row2[j]);
            m128_add_dpbusd_epi32(sum3, in, row3[j]);
          }

          *outptr = m128_haddx4(sum0, sum1, sum2, sum3, bias);
        }
      }
      else if constexpr (kOutputDimensions == 1)
      {
        __m128i sum0 = _mm_setzero_si128();

        const auto row0 = reinterpret_cast<const __m128i*>(&weights_[0]);

        for (int j = 0; j < (int)kNumChunks; j += 1)
        {
          const __m128i in = input_vector[j];

          m128_add_dpbusd_epi32(sum0, in, row0[j]);
        }

        output[0] = m128_hadd(sum0, biases_[0]);
      }
      else
      {
        // This case can never happen because kOutputDimensions
        // is always 1 or a multiple of kSimdWidth.
        assert(false);
      }

#else

// Use old implementation for the other architectures.

      auto output = reinterpret_cast<OutputType*>(buffer);

#if defined(USE_SSE2)
      constexpr IndexType kNumChunks = kPaddedInputDimensions / kSimdWidth;
#ifndef USE_SSSE3
      const __m128i kZeros = _mm_setzero_si128();
#else
      const __m128i kOnes = _mm_set1_epi16(1);
#endif
      const auto input_vector = reinterpret_cast<const __m128i*>(input);

#elif defined(USE_MMX)
      constexpr IndexType kNumChunks = kPaddedInputDimensions / kSimdWidth;
      const __m64 kZeros = _mm_setzero_si64();
      const auto input_vector = reinterpret_cast<const __m64*>(input);

#elif defined(USE_NEON)
      constexpr IndexType kNumChunks = kPaddedInputDimensions / kSimdWidth;
      const auto input_vector = reinterpret_cast<const int8x8_t*>(input);
#endif

      for (IndexType i = 0; i < kOutputDimensions; ++i) {
        const IndexType offset = i * kPaddedInputDimensions;

#if defined(USE_SSE2)
        __m128i sum_lo = _mm_cvtsi32_si128(biases_[i]);
        __m128i sum_hi = kZeros;
        const auto row = reinterpret_cast<const __m128i*>(&weights_[offset]);
        for (IndexType j = 0; j < kNumChunks; ++j) {
          __m128i row_j = _mm_load_si128(&row[j]);
          __m128i input_j = _mm_load_si128(&input_vector[j]);
          __m128i row_signs = _mm_cmpgt_epi8(kZeros, row_j);
          __m128i extended_row_lo = _mm_unpacklo_epi8(row_j, row_signs);
          __m128i extended_row_hi = _mm_unpackhi_epi8(row_j, row_signs);
          __m128i extended_input_lo = _mm_unpacklo_epi8(input_j, kZeros);
          __m128i extended_input_hi = _mm_unpackhi_epi8(input_j, kZeros);
          __m128i product_lo = _mm_madd_epi16(extended_row_lo, extended_input_lo);
          __m128i product_hi = _mm_madd_epi16(extended_row_hi, extended_input_hi);
          sum_lo = _mm_add_epi32(sum_lo, product_lo);
          sum_hi = _mm_add_epi32(sum_hi, product_hi);
        }
        __m128i sum = _mm_add_epi32(sum_lo, sum_hi);
        __m128i sum_high_64 = _mm_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2));
        sum = _mm_add_epi32(sum, sum_high_64);
        __m128i sum_second_32 = _mm_shufflelo_epi16(sum, _MM_SHUFFLE(1, 0, 3, 2));
        sum = _mm_add_epi32(sum, sum_second_32);
        output[i] = _mm_cvtsi128_si32(sum);

#elif defined(USE_MMX)
        __m64 sum_lo = _mm_cvtsi32_si64(biases_[i]);
        __m64 sum_hi = kZeros;
        const auto row = reinterpret_cast<const __m64*>(&weights_[offset]);
        for (IndexType j = 0; j < kNumChunks; ++j) {
          __m64 row_j = row[j];
          __m64 input_j = input_vector[j];
          __m64 row_signs = _mm_cmpgt_pi8(kZeros, row_j);
          __m64 extended_row_lo = _mm_unpacklo_pi8(row_j, row_signs);
          __m64 extended_row_hi = _mm_unpackhi_pi8(row_j, row_signs);
          __m64 extended_input_lo = _mm_unpacklo_pi8(input_j, kZeros);
          __m64 extended_input_hi = _mm_unpackhi_pi8(input_j, kZeros);
          __m64 product_lo = _mm_madd_pi16(extended_row_lo, extended_input_lo);
          __m64 product_hi = _mm_madd_pi16(extended_row_hi, extended_input_hi);
          sum_lo = _mm_add_pi32(sum_lo, product_lo);
          sum_hi = _mm_add_pi32(sum_hi, product_hi);
        }
        __m64 sum = _mm_add_pi32(sum_lo, sum_hi);
        sum = _mm_add_pi32(sum, _mm_unpackhi_pi32(sum, sum));
        output[i] = _mm_cvtsi64_si32(sum);

#elif defined(USE_NEON)
        int32x4_t sum = {biases_[i]};
        const auto row = reinterpret_cast<const int8x8_t*>(&weights_[offset]);
        for (IndexType j = 0; j < kNumChunks; ++j) {
          int16x8_t product = vmull_s8(input_vector[j * 2], row[j * 2]);
          product = vmlal_s8(product, input_vector[j * 2 + 1], row[j * 2 + 1]);
          sum = vpadalq_s16(sum, product);
        }
        output[i] = sum[0] + sum[1] + sum[2] + sum[3];

#else
        OutputType sum = biases_[i];
        for (IndexType j = 0; j < kInputDimensions; ++j) {
          sum += weights_[offset + j] * input[j];
        }
        output[i] = sum;
#endif

      }
#if defined(USE_MMX)
      _mm_empty();
#endif

#endif

      return output;
    }

   private:
    using BiasType = OutputType;
    using WeightType = std::int8_t;

    PreviousLayer previous_layer_;

    alignas(kCacheLineSize) BiasType biases_[kOutputDimensions];
    alignas(kCacheLineSize)
        WeightType weights_[kOutputDimensions * kPaddedInputDimensions];
  };

}  // namespace Eval::NNUE::Layers

#endif // #ifndef NNUE_LAYERS_AFFINE_TRANSFORM_H_INCLUDED
