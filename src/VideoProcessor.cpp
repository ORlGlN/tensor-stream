#include "VideoProcessor.h"
#include "Common.h"

void saveFrame(AVFrame *avFrame, FILE* dump) {
	
	uint8_t *frame = avFrame->data[0];
	for (uint32_t i = 0; i < avFrame->height; i++) {
		int sts = fwrite(frame, avFrame->width * avFrame->channels, 1, dump);
		if (avFrame->linesize[0])
			frame += avFrame->channels * avFrame->linesize[0];
		else
			frame += avFrame->channels * avFrame->width;
	}
	
	if (avFrame->format == AV_PIX_FMT_NV12) {
		uint8_t *frame = avFrame->data[1];
		for (uint32_t i = 0; i < avFrame->height / 2; i++) {
			fwrite(frame, avFrame->width * avFrame->channels, 1, dump);
			if (avFrame->linesize[1])
				frame += avFrame->channels * avFrame->linesize[1];
			else
				frame += avFrame->channels * avFrame->width;
		}
	}
	
	fflush(dump);
}

int VideoProcessor::DumpFrame(AVFrame* output, std::shared_ptr<FILE> dumpFile) {
	//allocate buffers
	std::shared_ptr<uint8_t> rawData(new uint8_t[output->channels * output->width * output->height], std::default_delete<uint8_t[]>());
	output->data[0] = rawData.get();
	cudaError err = cudaMemcpy(output->data[0], output->opaque, output->channels * output->width * output->height * sizeof(unsigned char), cudaMemcpyDeviceToHost);
	CHECK_STATUS(err);
	saveFrame(output, dumpFile.get());
	return OK;
}

int VideoProcessor::Init(bool _enableDumps) {
	enableDumps = _enableDumps;

	cudaGetDeviceProperties(&prop, 0);
	for (int i = 0; i < maxConsumers; i++) {
		cudaStream_t stream;
		cudaStreamCreate(&stream);
		streamArr.push_back(std::make_pair(std::string("empty"), stream));
	}
	
	if (enableDumps) {
		for (int i = 0; i < maxConsumers; i++) {
			std::string fileName = std::string("Processed_") + std::to_string(i) + std::string(".yuv");
			dumpArr.push_back(std::make_pair(std::string("empty"), std::shared_ptr<FILE>(fopen(fileName.c_str(), "wb+"), std::fclose)));
		}
	}
	isClosed = false;
	return OK;
}

void resize(AVFrame* input, AVFrame* output) {
	int o_width = output->width;
	int o_height = output->height;
	uint8_t *rawData = new uint8_t[1* o_width * o_height];
	float x_ratio = ((float)(input->width - 1)) / o_width; //why -1? due to boundaries?
	float y_ratio = ((float)(input->height - 1)) / o_height;
	int offset = 0;
	for (int i = 0; i < o_height; i++) {
		for (int j = 0; j < o_width; j++) {
			int x = (int)(x_ratio * j); //it's coordinate of pixel in source image
			int y = (int)(y_ratio * i); //
			float x_w = (x_ratio * j) - x;
			float y_h = (y_ratio * i) - y;
			int index = y * input->linesize[0] + x; //index in source image

			// range is 0 to 255 thus bitwise AND with 0xff
			int A = input->data[0][index] & 0xff;
			int B = input->data[0][index + 1] & 0xff;
			int C = input->data[0][index + input->linesize[0]] & 0xff;
			int D = input->data[0][index + input->linesize[0] + 1] & 0xff;

			// Y = A(1-w)(1-h) + B(w)(1-h) + C(h)(1-w) + Dwh
			int gray = (int)(
				A*(1 - x_w)*(1 - y_h) + B * (x_w)*(1 - y_h) +
				C * (y_h)*(1 - x_w) + D * (x_w*y_h)
				);

			rawData[offset++] = gray;
		}
	}
	output->data[0] = rawData;
}

void resizeNV12(AVFrame* input, AVFrame* output) {
	int o_width = output->width;
	int o_height = output->height;
	uint8_t *YData = new uint8_t[1 * o_width * o_height];
	uint8_t *UVData = new uint8_t[1 * o_width * o_height / 2];
	float x_ratio = ((float)(input->width - 1)) / o_width; //why -1? due to boundaries?
	float y_ratio = ((float)(input->height - 1)) / o_height;
	int offset = 0;
	for (int i = 0; i < o_height; i++) {
		for (int j = 0; j < o_width; j++) {
			int x = (int)(x_ratio * j); //it's coordinate of pixel in source image
			int y = (int)(y_ratio * i); //
			float x_w = (x_ratio * j) - x;
			float y_h = (y_ratio * i) - y;
			int index = y * input->linesize[0] + x; //index in source image

			// range is 0 to 255 thus bitwise AND with 0xff
			int A = input->data[0][index] & 0xff;
			int B = input->data[0][index + 1] & 0xff;
			int C = input->data[0][index + input->linesize[0]] & 0xff;
			int D = input->data[0][index + input->linesize[0] + 1] & 0xff;

			// Y = A(1-w)(1-h) + B(w)(1-h) + C(h)(1-w) + Dwh
			int gray = (int)(
				A*(1 - x_w)*(1 - y_h) + B * (x_w)*(1 - y_h) +
				C * (y_h)*(1 - x_w) + D * (x_w*y_h)
				);

			YData[offset++] = gray;

		}
	}
	output->data[0] = rawData;
}

int VideoProcessor::Convert(AVFrame* input, AVFrame* output, VPPParameters& format, std::string consumerName) {
	/*
	Should decide which method call
	*/
	cudaStream_t stream;
	int sts = OK;
	{
		std::unique_lock<std::mutex> locker(streamSync);
		stream = findFree<cudaStream_t>(consumerName, streamArr);
	}

	output->width = format.width;
	output->height = format.height;
	switch (format.dstFourCC) {
	case (RGB24):
		{
			output->format = AV_PIX_FMT_RGB24;
			//TODO: remove once resize is ready
			output->width = input->width;
			output->height = input->height;
			//this field is used only for audio so let's write number of planes there
			output->channels = 3;
			sts = NV12ToRGB24(input, output, prop.maxThreadsPerBlock, &stream);
			CHECK_STATUS(sts);
			break;
		}
	case (BGR24):
		{
			output->format = AV_PIX_FMT_BGR24;
			//this field is used only for audio so let's write number of planes there
			output->channels = 3;
			sts = NV12ToBGR24(input, output, prop.maxThreadsPerBlock, &stream);
			CHECK_STATUS(sts);
			break;
		}
	case (Y800):
		{
			std::shared_ptr<FILE> dump(fopen("test.yuv", "ab+"), std::fclose);
			output->format = AV_PIX_FMT_GRAY8;
			output->channels = 1;
			uint8_t* data = new uint8_t[input->linesize[0]*input->height];
			cudaError err = cudaMemcpy(data, input->data[0], input->linesize[0]* input->height * 1, cudaMemcpyDeviceToHost);
			input->data[0] = data;
			uint8_t* data1 = new uint8_t[input->linesize[0] * input->height / 2];
			err = cudaMemcpy(data1, input->data[1], input->linesize[0] * (input->height / 2) * 1, cudaMemcpyDeviceToHost);
			input->data[1] = data1;
			input->format = AV_PIX_FMT_NV12;
			output->format = AV_PIX_FMT_NV12;
			input->channels = 1;
			saveFrame(input, dump.get());
			resize(input, output);
			err = cudaMalloc(&output->opaque, output->channels * output->width * output->height * sizeof(unsigned char));
			err = cudaMemcpy(output->opaque, output->data[0], output->width * output->height * output->channels, cudaMemcpyHostToDevice);
			delete[] output->data[0];
			/*
			//NV12 has one plane with Y only component, so need just copy first plane
			cudaError err = cudaMalloc(&output->opaque, output->channels * output->width * output->height * sizeof(unsigned char));
			CHECK_STATUS(err);
			err = cudaMemcpy2D(output->opaque, output->width, input->data[0], input->linesize[0], output->width, output->height, cudaMemcpyDeviceToDevice);
			CHECK_STATUS(err);
			*/
			break;
		}
	}
	if (enableDumps) {
		std::shared_ptr<FILE> dumpFile;
		{
			std::unique_lock<std::mutex> locker(dumpSync);
			dumpFile = findFree<std::shared_ptr<FILE> >(consumerName, dumpArr);
		}
		DumpFrame(output, dumpFile);
	}
	av_frame_unref(input);
	return sts;
}

void VideoProcessor::Close() {
	if (isClosed)
		return;
	isClosed = true;
}
