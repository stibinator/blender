/*
 * Copyright 2011-2017 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

CCL_NAMESPACE_BEGIN

/* First step of the shadow prefiltering, performs the shadow division and stores all data
 * in a nice and easy rectangular array that can be passed to the NLM filter.
 *
 * Calculates:
 * unfiltered: Contains the two half images of the shadow feature pass
 * sampleVariance: The sample-based variance calculated in the kernel. Note: This calculation is biased in general, and especially here since the variance of the ratio can only be approximated.
 * sampleVarianceV: Variance of the sample variance estimation, quite noisy (since it's essentially the buffer variance of the two variance halves)
 * bufferVariance: The buffer-based variance of the shadow feature. Unbiased, but quite noisy.
 */
ccl_device void kernel_filter_divide_shadow(int sample,
                                            ccl_global TilesInfo *tiles,
                                            int x, int y,
                                            ccl_global float *unfilteredA,
                                            ccl_global float *unfilteredB,
                                            ccl_global float *sampleVariance,
                                            ccl_global float *sampleVarianceV,
                                            ccl_global float *bufferVariance,
                                            int4 rect,
                                            int buffer_pass_stride,
                                            int buffer_denoising_offset,
                                            bool use_split_variance)
{
	int xtile = (x < tiles->x[1])? 0: ((x < tiles->x[2])? 1: 2);
	int ytile = (y < tiles->y[1])? 0: ((y < tiles->y[2])? 1: 2);
	int tile = ytile*3+xtile;

	int offset = tiles->offsets[tile];
	int stride = tiles->strides[tile];
	ccl_global float ccl_restrict_ptr center_buffer = (ccl_global float*) tiles->buffers[tile];
	center_buffer += (y*stride + x + offset)*buffer_pass_stride;
	center_buffer += buffer_denoising_offset + 14;

	int buffer_w = align_up(rect.z - rect.x, 4);
	int idx = (y-rect.y)*buffer_w + (x - rect.x);
	unfilteredA[idx] = center_buffer[1] / max(center_buffer[0], 1e-7f);
	unfilteredB[idx] = center_buffer[4] / max(center_buffer[3], 1e-7f);

	float varA = center_buffer[2];
	float varB = center_buffer[5];
	int odd_sample = (sample+1)/2;
	int even_sample = sample/2;
	if(use_split_variance) {
		varA = max(0.0f, varA - unfilteredA[idx]*unfilteredA[idx]*odd_sample);
		varB = max(0.0f, varB - unfilteredB[idx]*unfilteredB[idx]*even_sample);
	}
	varA /= (odd_sample - 1);
	varB /= (even_sample - 1);

	sampleVariance[idx]  = 0.5f*(varA + varB) / sample;
	sampleVarianceV[idx] = 0.5f * (varA - varB) * (varA - varB) / (sample*sample);
	bufferVariance[idx]  = 0.5f * (unfilteredA[idx] - unfilteredB[idx]) * (unfilteredA[idx] - unfilteredB[idx]);
}

/* Load a regular feature from the render buffers into the denoise buffer.
 * Parameters:
 * - sample: The sample amount in the buffer, used to normalize the buffer.
 * - m_offset, v_offset: Render Buffer Pass offsets of mean and variance of the feature.
 * - x, y: Current pixel
 * - mean, variance: Target denoise buffers.
 * - rect: The prefilter area (lower pixels inclusive, upper pixels exclusive).
 */
ccl_device void kernel_filter_get_feature(int sample, 
                                          ccl_global TilesInfo *tiles,
                                          int m_offset, int v_offset,
                                          int x, int y,
                                          ccl_global float *mean,
                                          ccl_global float *variance,
                                          int4 rect, int buffer_pass_stride,
                                          int buffer_denoising_offset,
                                          bool use_split_variance)
{
	int xtile = (x < tiles->x[1])? 0: ((x < tiles->x[2])? 1: 2);
	int ytile = (y < tiles->y[1])? 0: ((y < tiles->y[2])? 1: 2);
	int tile = ytile*3+xtile;
	ccl_global float *center_buffer = ((ccl_global float*) tiles->buffers[tile]) + (tiles->offsets[tile] + y*tiles->strides[tile] + x)*buffer_pass_stride + buffer_denoising_offset;

	int buffer_w = align_up(rect.z - rect.x, 4);
	int idx = (y-rect.y)*buffer_w + (x - rect.x);

	mean[idx] = center_buffer[m_offset] / sample;
	if(use_split_variance) {
		variance[idx] = max(0.0f, (center_buffer[v_offset] - mean[idx]*mean[idx]*sample) / (sample * (sample-1)));
	}
	else {
		variance[idx] = center_buffer[v_offset] / (sample * (sample-1));
	}
}

/* Combine A/B buffers.
 * Calculates the combined mean and the buffer variance. */
ccl_device void kernel_filter_combine_halves(int x, int y,
                                             ccl_global float *mean,
                                             ccl_global float *variance,
                                             ccl_global float *a,
                                             ccl_global float *b,
                                             int4 rect, int r)
{
	int buffer_w = align_up(rect.z - rect.x, 4);
	int idx = (y-rect.y)*buffer_w + (x - rect.x);

	if(mean)     mean[idx] = 0.5f * (a[idx]+b[idx]);
	if(variance) {
		if(r == 0) variance[idx] = 0.25f * (a[idx]-b[idx])*(a[idx]-b[idx]);
		else {
			variance[idx] = 0.0f;
			float values[25];
			int numValues = 0;
			for(int py = max(y-r, rect.y); py < min(y+r+1, rect.w); py++) {
				for(int px = max(x-r, rect.x); px < min(x+r+1, rect.z); px++) {
					int pidx = (py-rect.y)*buffer_w + (px-rect.x);
					values[numValues++] = 0.25f * (a[pidx]-b[pidx])*(a[pidx]-b[pidx]);
				}
			}
			/* Insertion-sort the variances (fast enough for 25 elements). */
			for(int i = 1; i < numValues; i++) {
				float v = values[i];
				int j;
				for(j = i-1; j >= 0 && values[j] > v; j--)
					values[j+1] = values[j];
				values[j+1] = v;
			}
			variance[idx] = values[(7*numValues)/8];
		}
	}
}

CCL_NAMESPACE_END
