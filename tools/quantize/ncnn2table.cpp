// Tencent is pleased to support the open source community by making ncnn available.
//
// author:BUG1989 (https://github.com/BUG1989/) Long-term support.
// author:JansonZhu (https://github.com/JansonZhu) Implemented the function of entropy calibration.
//
// Copyright (C) 2019 BUG1989. All rights reserved.
// Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_DEPRECATE
#endif

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// #include <algorithm>
// #include <map>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <string>
#include <vector>

// ncnn public header
#include "benchmark.h"
#include "cpu.h"
#include "net.h"

// ncnn private header
#include "layer/convolution.h"
#include "layer/convolutiondepthwise.h"
#include "layer/innerproduct.h"

class QuantBlobStat
{
public:
    QuantBlobStat()
    {
        threshold = 0.f;
        absmax = 0.f;
        total = 0;
    }

public:
    float threshold;
    float absmax;

    // ACIQ
    int total;

    // KL
    std::vector<uint64_t> histogram;
    std::vector<float> histogram_normed;
};

class QuantNet : public ncnn::Net
{
public:
    QuantNet();

    std::vector<ncnn::Blob>& blobs;
    std::vector<ncnn::Layer*>& layers;

public:
    std::vector<std::vector<std::string> > listspaths;
    std::vector<std::vector<float> > means;
    std::vector<std::vector<float> > norms;
    std::vector<std::vector<int> > shapes;
    std::vector<int> type_to_pixels;
    int quantize_num_threads;

public:
    int init();
    int save_table(const char* tablepath);
    int quantize_KL();
    int quantize_ACIQ();

public:
    std::vector<int> input_blobs;
    std::vector<int> conv_layers;
    std::vector<int> conv_bottom_blobs;

    // result
    std::vector<ncnn::Mat> weight_scales;
    std::vector<ncnn::Mat> bottom_blobs_scales;
};

QuantNet::QuantNet()
    : blobs(mutable_blobs()), layers(mutable_layers())
{
    quantize_num_threads = 8;
}

int QuantNet::init()
{
    // find all input layers
    for (int i = 0; i < (int)layers.size(); i++)
    {
        const ncnn::Layer* layer = layers[i];
        if (layer->type == "Input")
        {
            input_blobs.push_back(layer->tops[0]);
        }
    }

    // find all conv layers
    for (int i = 0; i < (int)layers.size(); i++)
    {
        const ncnn::Layer* layer = layers[i];
        if (layer->type == "Convolution" || layer->type == "ConvolutionDepthWise" || layer->type == "InnerProduct")
        {
            conv_layers.push_back(i);
            conv_bottom_blobs.push_back(layer->bottoms[0]);
        }
    }

    const int conv_layer_count = (int)conv_layers.size();
    const int conv_bottom_blob_count = (int)conv_bottom_blobs.size();

    weight_scales.resize(conv_layer_count);
    bottom_blobs_scales.resize(conv_bottom_blob_count);

    return 0;
}

int QuantNet::save_table(const char* tablepath)
{
    FILE* fp = fopen(tablepath, "wb");
    if (!fp)
    {
        fprintf(stderr, "fopen %s failed\n", tablepath);
        return -1;
    }

    const int conv_layer_count = (int)conv_layers.size();
    const int conv_bottom_blob_count = (int)conv_bottom_blobs.size();

    for (int i = 0; i < conv_layer_count; i++)
    {
        const ncnn::Mat& weight_scale = weight_scales[i];

        fprintf(fp, "%s_param_0 ", layers[conv_layers[i]]->name.c_str());
        for (int j = 0; j < weight_scale.w; j++)
        {
            fprintf(fp, "%f ", weight_scale[j]);
        }
        fprintf(fp, "\n");
    }

    for (int i = 0; i < conv_bottom_blob_count; i++)
    {
        const ncnn::Mat& bottom_blobs_scale = bottom_blobs_scales[i];

        fprintf(fp, "%s ", layers[conv_layers[i]]->name.c_str());
        for (int j = 0; j < bottom_blobs_scale.w; j++)
        {
            fprintf(fp, "%f ", bottom_blobs_scale[j]);
        }
        fprintf(fp, "\n");
    }

    fclose(fp);

    fprintf(stderr, "ncnn int8 calibration table create success, best wish for your int8 inference has a low accuracy loss...\\(^0^)/...233...\n");

    return 0;
}

static float compute_kl_divergence(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t length = a.size();

    float result = 0;
    for (size_t i = 0; i < length; i++)
    {
        result += a[i] * log(a[i] / b[i]);
    }

    return result;
}

int QuantNet::quantize_KL()
{
    const int input_blob_count = (int)input_blobs.size();
    const int conv_layer_count = (int)conv_layers.size();
    const int conv_bottom_blob_count = (int)conv_bottom_blobs.size();
    const int image_count = (int)listspaths[0].size();

    const int num_histogram_bins = 2048;

    // initialize conv weight scales
    #pragma omp parallel for num_threads(quantize_num_threads)
    for (int i = 0; i < conv_layer_count; i++)
    {
        const ncnn::Layer* layer = layers[conv_layers[i]];

        if (layer->type == "Convolution")
        {
            const ncnn::Convolution* convolution = (const ncnn::Convolution*)(layer);

            const int num_output = convolution->num_output;
            const int kernel_w = convolution->kernel_w;
            const int kernel_h = convolution->kernel_h;
            const int dilation_w = convolution->dilation_w;
            const int dilation_h = convolution->dilation_h;
            const int stride_w = convolution->stride_w;
            const int stride_h = convolution->stride_h;

            const int weight_data_size_output = convolution->weight_data_size / num_output;

            // int8 winograd F43 needs weight data to use 6bit quantization
            // TODO proper condition for winograd 3x3 int8
            bool quant_6bit = false;
            if (kernel_w == 3 && kernel_h == 3 && dilation_w == 1 && dilation_h == 1 && stride_w == 1 && stride_h == 1)
                quant_6bit = true;

            weight_scales[i].create(num_output);

            for (int n = 0; n < num_output; n++)
            {
                const ncnn::Mat weight_data_n = convolution->weight_data.range(weight_data_size_output * n, weight_data_size_output);

                float absmax = 0.f;
                for (int k = 0; k < weight_data_size_output; k++)
                {
                    absmax = std::max(absmax, fabs(weight_data_n[k]));
                }

                if (quant_6bit)
                {
                    weight_scales[i][n] = 31 / absmax;
                }
                else
                {
                    weight_scales[i][n] = 127 / absmax;
                }
            }
        }

        if (layer->type == "ConvolutionDepthWise")
        {
            const ncnn::ConvolutionDepthWise* convolutiondepthwise = static_cast<const ncnn::ConvolutionDepthWise*>(layer);

            const int group = convolutiondepthwise->group;
            const int weight_data_size_output = convolutiondepthwise->weight_data_size / group;

            std::vector<float> scales;

            weight_scales[i].create(group);

            for (int n = 0; n < group; n++)
            {
                const ncnn::Mat weight_data_n = convolutiondepthwise->weight_data.range(weight_data_size_output * n, weight_data_size_output);

                float absmax = 0.f;
                for (int k = 0; k < weight_data_size_output; k++)
                {
                    absmax = std::max(absmax, fabs(weight_data_n[k]));
                }

                weight_scales[i][n] = 127 / absmax;
            }
        }

        if (layer->type == "InnerProduct")
        {
            const ncnn::InnerProduct* innerproduct = static_cast<const ncnn::InnerProduct*>(layer);

            const int num_output = innerproduct->num_output;
            const int weight_data_size_output = innerproduct->weight_data_size / num_output;

            weight_scales[i].create(num_output);

            for (int n = 0; n < num_output; n++)
            {
                const ncnn::Mat weight_data_n = innerproduct->weight_data.range(weight_data_size_output * n, weight_data_size_output);

                float absmax = 0.f;
                for (int k = 0; k < weight_data_size_output; k++)
                {
                    absmax = std::max(absmax, fabs(weight_data_n[k]));
                }

                weight_scales[i][n] = 127 / absmax;
            }
        }
    }

    // count the absmax
    std::vector<QuantBlobStat> quant_blob_stats(conv_bottom_blob_count);
    #pragma omp parallel for num_threads(quantize_num_threads)
    for (int i = 0; i < image_count; i++)
    {
        ncnn::Extractor ex = create_extractor();

        for (int j = 0; j < input_blob_count; j++)
        {
            const std::string& imagepath = listspaths[j][i];
            const std::vector<int>& shape = shapes[j];
            const int type_to_pixel = type_to_pixels[j];
            const std::vector<float>& mean_vals = means[j];
            const std::vector<float>& norm_vals = norms[j];

            int pixel_convert_type = ncnn::Mat::PIXEL_BGR;
            if (type_to_pixel != pixel_convert_type)
            {
                pixel_convert_type = pixel_convert_type | (type_to_pixel << ncnn::Mat::PIXEL_CONVERT_SHIFT);
            }
            const int target_w = shape[0];
            const int target_h = shape[1];

            cv::Mat bgr = cv::imread(imagepath, 1);

            ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr.data, pixel_convert_type, bgr.cols, bgr.rows, target_w, target_h);

            in.substract_mean_normalize(mean_vals.data(), norm_vals.data());

            ex.input(input_blobs[j], in);
        }

        for (int j = 0; j < conv_bottom_blob_count; j++)
        {
            ncnn::Mat out;
            ex.extract(conv_bottom_blobs[j], out);

            // count absmax
            {
                float absmax = 0.f;

                const int outc = out.c;
                const int outsize = out.w * out.h;
                for (int p = 0; p < outc; p++)
                {
                    const float* ptr = out.channel(p);
                    for (int k = 0; k < outsize; k++)
                    {
                        absmax = std::max(absmax, fabs(ptr[k]));
                    }
                }

                #pragma omp critical
                {
                    QuantBlobStat& stat = quant_blob_stats[j];
                    stat.absmax = std::max(stat.absmax, absmax);
                }
            }
        }
    }

    // initialize histogram
    #pragma omp parallel for num_threads(quantize_num_threads)
    for (int i = 0; i < conv_bottom_blob_count; i++)
    {
        QuantBlobStat& stat = quant_blob_stats[i];

        stat.histogram.resize(num_histogram_bins, 0);
        stat.histogram_normed.resize(num_histogram_bins, 0);
    }

    // build histogram
    #pragma omp parallel for num_threads(quantize_num_threads)
    for (int i = 0; i < image_count; i++)
    {
        ncnn::Extractor ex = create_extractor();

        for (int j = 0; j < input_blob_count; j++)
        {
            const std::string& imagepath = listspaths[j][i];
            const std::vector<int>& shape = shapes[j];
            const int type_to_pixel = type_to_pixels[j];
            const std::vector<float>& mean_vals = means[j];
            const std::vector<float>& norm_vals = norms[j];

            int pixel_convert_type = ncnn::Mat::PIXEL_BGR;
            if (type_to_pixel != pixel_convert_type)
            {
                pixel_convert_type = pixel_convert_type | (type_to_pixel << ncnn::Mat::PIXEL_CONVERT_SHIFT);
            }
            const int target_w = shape[0];
            const int target_h = shape[1];

            cv::Mat bgr = cv::imread(imagepath, 1);

            ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr.data, pixel_convert_type, bgr.cols, bgr.rows, target_w, target_h);

            in.substract_mean_normalize(mean_vals.data(), norm_vals.data());

            ex.input(input_blobs[j], in);
        }

        for (int j = 0; j < conv_bottom_blob_count; j++)
        {
            ncnn::Mat out;
            ex.extract(conv_bottom_blobs[j], out);

            // count histogram bin
            {
                const float absmax = quant_blob_stats[j].absmax;

                std::vector<uint64_t> histogram(num_histogram_bins, 0);

                const int outc = out.c;
                const int outsize = out.w * out.h;
                for (int p = 0; p < outc; p++)
                {
                    const float* ptr = out.channel(p);
                    for (int k = 0; k < outsize; k++)
                    {
                        if (ptr[k] == 0.f)
                            continue;

                        const int index = std::min((int)(fabs(ptr[k]) / absmax * num_histogram_bins), (num_histogram_bins - 1));

                        histogram[index] += 1;
                    }
                }

                #pragma omp critical
                {
                    QuantBlobStat& stat = quant_blob_stats[j];

                    for (int k = 0; k < num_histogram_bins; k++)
                    {
                        stat.histogram[k] += histogram[k];
                    }
                }
            }
        }
    }

    // using kld to find the best threshold value
    #pragma omp parallel for num_threads(quantize_num_threads)
    for (int i = 0; i < conv_bottom_blob_count; i++)
    {
        QuantBlobStat& stat = quant_blob_stats[i];

        // normalize histogram bin
        {
            uint64_t sum = 0;
            for (int j = 0; j < num_histogram_bins; j++)
            {
                sum += stat.histogram[j];
            }

            for (int j = 0; j < num_histogram_bins; j++)
            {
                stat.histogram_normed[j] = (float)(stat.histogram[j] / (double)sum);
            }
        }

        const int target_bin = 128;

        int target_threshold = target_bin;
        float min_kl_divergence = FLT_MAX;

        for (int threshold = target_bin; threshold < num_histogram_bins; threshold++)
        {
            const float kl_eps = 0.0001f;

            std::vector<float> clip_distribution(threshold, kl_eps);
            {
                for (int j = 0; j < threshold; j++)
                {
                    clip_distribution[j] += stat.histogram_normed[j];
                }
                for (int j = threshold; j < num_histogram_bins; j++)
                {
                    clip_distribution[threshold - 1] += stat.histogram_normed[j];
                }
            }

            const float num_per_bin = (float)threshold / target_bin;

            std::vector<float> quantize_distribution(target_bin, 0.f);
            {
                {
                    const float end = num_per_bin;

                    const int right_lower = (int)floor(end);
                    const float right_scale = end - right_lower;

                    if (right_scale > 0)
                    {
                        quantize_distribution[0] += right_scale * stat.histogram_normed[right_lower];
                    }

                    for (int k = 0; k < right_lower; k++)
                    {
                        quantize_distribution[0] += stat.histogram_normed[k];
                    }

                    quantize_distribution[0] /= right_lower + right_scale;
                }
                for (int j = 1; j < target_bin - 1; j++)
                {
                    const float start = j * num_per_bin;
                    const float end = (j + 1) * num_per_bin;

                    const int left_upper = (int)ceil(start);
                    const float left_scale = left_upper - start;

                    const int right_lower = (int)floor(end);
                    const float right_scale = end - right_lower;

                    if (left_scale > 0)
                    {
                        quantize_distribution[j] += left_scale * stat.histogram_normed[left_upper - 1];
                    }

                    if (right_scale > 0)
                    {
                        quantize_distribution[j] += right_scale * stat.histogram_normed[right_lower];
                    }

                    for (int k = left_upper; k < right_lower; k++)
                    {
                        quantize_distribution[j] += stat.histogram_normed[k];
                    }

                    quantize_distribution[j] /= right_lower - left_upper + left_scale + right_scale;
                }
                {
                    const float start = threshold - num_per_bin;

                    const int left_upper = (int)ceil(start);
                    const float left_scale = left_upper - start;

                    if (left_scale > 0)
                    {
                        quantize_distribution[target_bin - 1] += left_scale * stat.histogram_normed[left_upper - 1];
                    }

                    for (int k = left_upper; k < threshold; k++)
                    {
                        quantize_distribution[target_bin - 1] += stat.histogram_normed[k];
                    }

                    quantize_distribution[target_bin - 1] /= threshold - left_upper + left_scale;
                }
            }

            std::vector<float> expand_distribution(threshold, kl_eps);
            {
                {
                    const float end = num_per_bin;

                    const int right_lower = (int)floor(end);
                    const float right_scale = end - right_lower;

                    if (right_scale > 0)
                    {
                        expand_distribution[right_lower] += right_scale * quantize_distribution[0];
                    }

                    for (int k = 0; k < right_lower; k++)
                    {
                        expand_distribution[k] += quantize_distribution[0];
                    }
                }
                for (int j = 1; j < target_bin - 1; j++)
                {
                    const float start = j * num_per_bin;
                    const float end = (j + 1) * num_per_bin;

                    const int left_upper = (int)ceil(start);
                    const float left_scale = left_upper - start;

                    const int right_lower = (int)floor(end);
                    const float right_scale = end - right_lower;

                    if (left_scale > 0)
                    {
                        expand_distribution[left_upper - 1] += left_scale * quantize_distribution[j];
                    }

                    if (right_scale > 0)
                    {
                        expand_distribution[right_lower] += right_scale * quantize_distribution[j];
                    }

                    for (int k = left_upper; k < right_lower; k++)
                    {
                        expand_distribution[k] += quantize_distribution[j];
                    }
                }
                {
                    const float start = threshold - num_per_bin;

                    const int left_upper = (int)ceil(start);
                    const float left_scale = left_upper - start;

                    if (left_scale > 0)
                    {
                        expand_distribution[left_upper - 1] += left_scale * quantize_distribution[target_bin - 1];
                    }

                    for (int k = left_upper; k < threshold; k++)
                    {
                        expand_distribution[k] += quantize_distribution[target_bin - 1];
                    }
                }
            }

            // kl
            const float kl_divergence = compute_kl_divergence(clip_distribution, expand_distribution);

            // the best num of bin
            if (kl_divergence < min_kl_divergence)
            {
                min_kl_divergence = kl_divergence;
                target_threshold = threshold;
            }
        }

        stat.threshold = (target_threshold + 0.5f) * stat.absmax / num_histogram_bins;
        float scale = 127 / stat.threshold;

        bottom_blobs_scales[i].create(1);
        bottom_blobs_scales[i][0] = scale;
    }

    // some info
    for (int i = 0; i < conv_bottom_blob_count; i++)
    {
        const QuantBlobStat& stat = quant_blob_stats[i];

        float scale = 127 / stat.threshold;

        fprintf(stderr, "%-40s : max = %-15f  threshold = %-15f  scale = %-15f\n", layers[conv_layers[i]]->name.c_str(), stat.absmax, stat.threshold, scale);
    }

    return 0;
}

static float compute_aciq_gaussian_clip(float absmax, int N, int num_bits = 8)
{
    const float alpha_gaussian[8] = {0, 1.71063519, 2.15159277, 2.55913646, 2.93620062, 3.28691474, 3.6151146, 3.92403714};

    const double gaussian_const = (0.5 * 0.35) * (1 + sqrt(3.14159265358979323846 * log(4)));

    double std = (absmax * 2 * gaussian_const) / sqrt(2 * log(N));

    return (float)(alpha_gaussian[num_bits - 1] * std);
}

int QuantNet::quantize_ACIQ()
{
    const int input_blob_count = (int)input_blobs.size();
    const int conv_layer_count = (int)conv_layers.size();
    const int conv_bottom_blob_count = (int)conv_bottom_blobs.size();
    const int image_count = (int)listspaths[0].size();

    // initialize conv weight scales
    #pragma omp parallel for num_threads(quantize_num_threads)
    for (int i = 0; i < conv_layer_count; i++)
    {
        const ncnn::Layer* layer = layers[conv_layers[i]];

        if (layer->type == "Convolution")
        {
            const ncnn::Convolution* convolution = (const ncnn::Convolution*)(layer);

            const int num_output = convolution->num_output;
            const int kernel_w = convolution->kernel_w;
            const int kernel_h = convolution->kernel_h;
            const int dilation_w = convolution->dilation_w;
            const int dilation_h = convolution->dilation_h;
            const int stride_w = convolution->stride_w;
            const int stride_h = convolution->stride_h;

            const int weight_data_size_output = convolution->weight_data_size / num_output;

            // int8 winograd F43 needs weight data to use 6bit quantization
            // TODO proper condition for winograd 3x3 int8
            bool quant_6bit = false;
            if (kernel_w == 3 && kernel_h == 3 && dilation_w == 1 && dilation_h == 1 && stride_w == 1 && stride_h == 1)
                quant_6bit = true;

            weight_scales[i].create(num_output);

            for (int n = 0; n < num_output; n++)
            {
                const ncnn::Mat weight_data_n = convolution->weight_data.range(weight_data_size_output * n, weight_data_size_output);

                float absmax = 0.f;
                for (int k = 0; k < weight_data_size_output; k++)
                {
                    absmax = std::max(absmax, fabs(weight_data_n[k]));
                }

                if (quant_6bit)
                {
                    const float threshold = compute_aciq_gaussian_clip(absmax, weight_data_size_output, 6);
                    weight_scales[i][n] = 31 / threshold;
                }
                else
                {
                    const float threshold = compute_aciq_gaussian_clip(absmax, weight_data_size_output);
                    weight_scales[i][n] = 127 / threshold;
                }
            }
        }

        if (layer->type == "ConvolutionDepthWise")
        {
            const ncnn::ConvolutionDepthWise* convolutiondepthwise = static_cast<const ncnn::ConvolutionDepthWise*>(layer);

            const int group = convolutiondepthwise->group;
            const int weight_data_size_output = convolutiondepthwise->weight_data_size / group;

            std::vector<float> scales;

            weight_scales[i].create(group);

            for (int n = 0; n < group; n++)
            {
                const ncnn::Mat weight_data_n = convolutiondepthwise->weight_data.range(weight_data_size_output * n, weight_data_size_output);

                float absmax = 0.f;
                for (int k = 0; k < weight_data_size_output; k++)
                {
                    absmax = std::max(absmax, fabs(weight_data_n[k]));
                }

                const float threshold = compute_aciq_gaussian_clip(absmax, weight_data_size_output);
                weight_scales[i][n] = 127 / threshold;
            }
        }

        if (layer->type == "InnerProduct")
        {
            const ncnn::InnerProduct* innerproduct = static_cast<const ncnn::InnerProduct*>(layer);

            const int num_output = innerproduct->num_output;
            const int weight_data_size_output = innerproduct->weight_data_size / num_output;

            weight_scales[i].create(num_output);

            for (int n = 0; n < num_output; n++)
            {
                const ncnn::Mat weight_data_n = innerproduct->weight_data.range(weight_data_size_output * n, weight_data_size_output);

                float absmax = 0.f;
                for (int k = 0; k < weight_data_size_output; k++)
                {
                    absmax = std::max(absmax, fabs(weight_data_n[k]));
                }

                const float threshold = compute_aciq_gaussian_clip(absmax, weight_data_size_output);
                weight_scales[i][n] = 127 / threshold;
            }
        }
    }

    // count the absmax abssum
    std::vector<QuantBlobStat> quant_blob_stats(conv_bottom_blob_count);
    #pragma omp parallel for num_threads(quantize_num_threads)
    for (int i = 0; i < image_count; i++)
    {
        ncnn::Extractor ex = create_extractor();

        for (int j = 0; j < input_blob_count; j++)
        {
            const std::string& imagepath = listspaths[j][i];
            const std::vector<int>& shape = shapes[j];
            const int type_to_pixel = type_to_pixels[j];
            const std::vector<float>& mean_vals = means[j];
            const std::vector<float>& norm_vals = norms[j];

            int pixel_convert_type = ncnn::Mat::PIXEL_BGR;
            if (type_to_pixel != pixel_convert_type)
            {
                pixel_convert_type = pixel_convert_type | (type_to_pixel << ncnn::Mat::PIXEL_CONVERT_SHIFT);
            }
            const int target_w = shape[0];
            const int target_h = shape[1];

            cv::Mat bgr = cv::imread(imagepath, 1);

            ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr.data, pixel_convert_type, bgr.cols, bgr.rows, target_w, target_h);

            in.substract_mean_normalize(mean_vals.data(), norm_vals.data());

            ex.input(input_blobs[j], in);
        }

        for (int j = 0; j < conv_bottom_blob_count; j++)
        {
            ncnn::Mat out;
            ex.extract(conv_bottom_blobs[j], out);

            // count absmax
            {
                float absmax = 0.f;

                const int outc = out.c;
                const int outsize = out.w * out.h;
                for (int p = 0; p < outc; p++)
                {
                    const float* ptr = out.channel(p);
                    for (int k = 0; k < outsize; k++)
                    {
                        absmax = std::max(absmax, fabs(ptr[k]));
                    }
                }

                #pragma omp critical
                {
                    QuantBlobStat& stat = quant_blob_stats[j];
                    stat.absmax = std::max(stat.absmax, absmax);
                    stat.total = outc * outsize;
                }
            }
        }
    }

    // alpha gaussian
    #pragma omp parallel for num_threads(quantize_num_threads)
    for (int i = 0; i < conv_bottom_blob_count; i++)
    {
        QuantBlobStat& stat = quant_blob_stats[i];

        stat.threshold = compute_aciq_gaussian_clip(stat.absmax, stat.total);
        float scale = 127 / stat.threshold;

        bottom_blobs_scales[i].create(1);
        bottom_blobs_scales[i][0] = scale;
    }

    // some info
    for (int i = 0; i < conv_bottom_blob_count; i++)
    {
        const QuantBlobStat& stat = quant_blob_stats[i];

        float scale = 127 / stat.threshold;

        fprintf(stderr, "%-40s : max = %-15f  threshold = %-15f  scale = %-15f\n", layers[conv_layers[i]]->name.c_str(), stat.absmax, stat.threshold, scale);
    }

    return 0;
}

static std::vector<std::vector<std::string> > parse_comma_path_list(char* s)
{
    std::vector<std::vector<std::string> > aps;

    char* pch = strtok(s, ",");
    while (pch != NULL)
    {
        FILE* fp = fopen(pch, "rb");
        if (!fp)
        {
            fprintf(stderr, "fopen %s failed\n", pch);
            break;
        }

        std::vector<std::string> paths;

        // one filepath per line
        char line[1024];
        while (!feof(fp))
        {
            char* s = fgets(line, 1024, fp);
            if (!s)
                break;

            char filepath[256];
            int nscan = sscanf(line, "%255s", filepath);
            if (nscan != 1)
                continue;

            paths.push_back(std::string(filepath));
        }

        fclose(fp);

        aps.push_back(paths);

        pch = strtok(NULL, ",");
    }

    return aps;
}

static float vstr_to_float(const char vstr[16])
{
    double v = 0.0;

    const char* p = vstr;

    // sign
    bool sign = *p != '-';
    if (*p == '+' || *p == '-')
    {
        p++;
    }

    // digits before decimal point or exponent
    unsigned int v1 = 0;
    while (isdigit(*p))
    {
        v1 = v1 * 10 + (*p - '0');
        p++;
    }

    v = (double)v1;

    // digits after decimal point
    if (*p == '.')
    {
        p++;

        unsigned int pow10 = 1;
        unsigned int v2 = 0;

        while (isdigit(*p))
        {
            v2 = v2 * 10 + (*p - '0');
            pow10 *= 10;
            p++;
        }

        v += v2 / (double)pow10;
    }

    // exponent
    if (*p == 'e' || *p == 'E')
    {
        p++;

        // sign of exponent
        bool fact = *p != '-';
        if (*p == '+' || *p == '-')
        {
            p++;
        }

        // digits of exponent
        unsigned int expon = 0;
        while (isdigit(*p))
        {
            expon = expon * 10 + (*p - '0');
            p++;
        }

        double scale = 1.0;
        while (expon >= 8)
        {
            scale *= 1e8;
            expon -= 8;
        }
        while (expon > 0)
        {
            scale *= 10.0;
            expon -= 1;
        }

        v = fact ? v * scale : v / scale;
    }

    //     fprintf(stderr, "v = %f\n", v);
    return sign ? (float)v : (float)-v;
}

static std::vector<std::vector<float> > parse_comma_float_array_list(char* s)
{
    std::vector<std::vector<float> > aaf;

    char* pch = strtok(s, "[]");
    while (pch != NULL)
    {
        // parse a,b,c
        char vstr[16];
        int nconsumed = 0;
        int nscan = sscanf(pch, "%15[^,]%n", vstr, &nconsumed);
        if (nscan == 1)
        {
            // ok we get array
            pch += nconsumed;

            std::vector<float> af;
            float v = vstr_to_float(vstr);
            af.push_back(v);

            nscan = sscanf(pch, ",%15[^,]%n", vstr, &nconsumed);
            while (nscan == 1)
            {
                pch += nconsumed;

                float v = vstr_to_float(vstr);
                af.push_back(v);

                nscan = sscanf(pch, ",%15[^,]%n", vstr, &nconsumed);
            }

            // array end
            aaf.push_back(af);
        }

        pch = strtok(NULL, "[]");
    }

    return aaf;
}

static std::vector<std::vector<int> > parse_comma_int_array_list(char* s)
{
    std::vector<std::vector<int> > aai;

    char* pch = strtok(s, "[]");
    while (pch != NULL)
    {
        // parse a,b,c
        int v;
        int nconsumed = 0;
        int nscan = sscanf(pch, "%d%n", &v, &nconsumed);
        if (nscan == 1)
        {
            // ok we get array
            pch += nconsumed;

            std::vector<int> ai;
            ai.push_back(v);

            nscan = sscanf(pch, ",%d%n", &v, &nconsumed);
            while (nscan == 1)
            {
                pch += nconsumed;

                ai.push_back(v);

                nscan = sscanf(pch, ",%d%n", &v, &nconsumed);
            }

            // array end
            aai.push_back(ai);
        }

        pch = strtok(NULL, "[]");
    }

    return aai;
}

static std::vector<int> parse_comma_pixel_type_list(char* s)
{
    std::vector<int> aps;

    char* pch = strtok(s, ",");
    while (pch != NULL)
    {
        // RAW/RGB/BGR/GRAY/RGBA/BGRA
        if (strcmp(pch, "RAW") == 0)
            aps.push_back(-233);
        if (strcmp(pch, "RGB") == 0)
            aps.push_back(ncnn::Mat::PIXEL_RGB);
        if (strcmp(pch, "BGR") == 0)
            aps.push_back(ncnn::Mat::PIXEL_BGR);
        if (strcmp(pch, "GRAY") == 0)
            aps.push_back(ncnn::Mat::PIXEL_GRAY);
        if (strcmp(pch, "RGBA") == 0)
            aps.push_back(ncnn::Mat::PIXEL_RGBA);
        if (strcmp(pch, "BGRA") == 0)
            aps.push_back(ncnn::Mat::PIXEL_BGRA);

        pch = strtok(NULL, ",");
    }

    return aps;
}

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "Usage: %s [ncnnparam] [ncnnbin] [list,...] [ncnntable] [(key=value)...]\n", argv[0]);
        fprintf(stderr, "  mean=[104.0,117.0,123.0],...\n");
        fprintf(stderr, "  norm=[1.0,1.0,1.0],...\n");
        fprintf(stderr, "  shape=[224,224,3],...\n");
        fprintf(stderr, "  pixel=RAW/RGB/BGR/GRAY/RGBA/BGRA,...\n");
        fprintf(stderr, "  thread=8\n");
        fprintf(stderr, "  method=kl/aciq/eq\n");
        fprintf(stderr, "Sample usage: %s squeezenet.param squeezenet.bin imagelist.txt squeezenet.table mean=[104.0,117.0,123.0] norm=[1.0,1.0,1.0] shape=[227,227,3] pixel=BGR method=kl\n", argv[0]);
        return -1;
    }

    const char* inparam = argv[1];
    const char* inbin = argv[2];
    char* lists = argv[3];
    const char* outtable = argv[4];

    ncnn::Option opt;
    opt.num_threads = 1;
    opt.use_fp16_packed = false;
    opt.use_fp16_storage = false;
    opt.use_fp16_arithmetic = false;

    QuantNet net;
    net.opt = opt;
    net.load_param(inparam);
    net.load_model(inbin);

    net.init();

    // load lists
    net.listspaths = parse_comma_path_list(lists);

    std::string method = "kl";

    for (int i = 5; i < argc; i++)
    {
        // key=value
        char* kv = argv[i];

        char* eqs = strchr(kv, '=');
        if (eqs == NULL)
        {
            fprintf(stderr, "unrecognized arg %s\n", kv);
            continue;
        }

        // split k v
        eqs[0] = '\0';
        const char* key = kv;
        char* value = eqs + 1;

        fprintf(stderr, "%s = %s\n", key, value);

        // load mean norm shape
        if (memcmp(key, "mean", 4) == 0)
            net.means = parse_comma_float_array_list(value);
        if (memcmp(key, "norm", 4) == 0)
            net.norms = parse_comma_float_array_list(value);
        if (memcmp(key, "shape", 5) == 0)
            net.shapes = parse_comma_int_array_list(value);
        if (memcmp(key, "pixel", 5) == 0)
            net.type_to_pixels = parse_comma_pixel_type_list(value);
        if (memcmp(key, "thread", 6) == 0)
            net.quantize_num_threads = atoi(value);
        if (memcmp(key, "method", 6) == 0)
            method = std::string(value);
    }

    // sanity check
    const size_t input_blob_count = net.input_blobs.size();
    if (net.listspaths.size() != input_blob_count)
    {
        fprintf(stderr, "expect %d lists, but got %d\n", (int)input_blob_count, (int)net.listspaths.size());
        return -1;
    }
    if (net.means.size() != input_blob_count)
    {
        fprintf(stderr, "expect %d means, but got %d\n", (int)input_blob_count, (int)net.means.size());
        return -1;
    }
    if (net.norms.size() != input_blob_count)
    {
        fprintf(stderr, "expect %d norms, but got %d\n", (int)input_blob_count, (int)net.norms.size());
        return -1;
    }
    if (net.shapes.size() != input_blob_count)
    {
        fprintf(stderr, "expect %d shapes, but got %d\n", (int)input_blob_count, (int)net.shapes.size());
        return -1;
    }
    if (net.type_to_pixels.size() != input_blob_count)
    {
        fprintf(stderr, "expect %d pixels, but got %d\n", (int)input_blob_count, (int)net.type_to_pixels.size());
        return -1;
    }
    if (net.quantize_num_threads < 0)
    {
        fprintf(stderr, "malformed thread %d\n", net.quantize_num_threads);
        return -1;
    }

    if (method == "kl")
    {
        net.quantize_KL();
    }
    else if (method == "aciq")
    {
        net.quantize_ACIQ();
    }
    else
    {
        fprintf(stderr, "not implemented yet !\n");
        fprintf(stderr, "unknown method %s, expect kl / aciq / eq\n", method.c_str());
        return -1;
    }

    net.save_table(outtable);

    return 0;
}
