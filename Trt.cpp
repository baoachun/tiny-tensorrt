/*
 * @Description: In User Settings Edit
 * @Author: your name
 * @Date: 2019-08-21 14:06:38
 * @LastEditTime: 2019-08-30 17:04:04
 * @LastEditors: zerollzeng
 */
#include "Trt.h"
#include "utils.h"
#include "spdlog/spdlog.h"
#include "Int8EntropyCalibrator.h"
#include "plugin/PluginFactory.h"

#include <string>
#include <vector>
#include <iostream>
#include <cassert>
#include <fstream>
#include <memory>

#include "NvInfer.h"
#include "NvCaffeParser.h"
#include "NvOnnxParser.h"

Trt::Trt() {
    TrtPluginParams params;
    mPluginFactory = new PluginFactory(params);
}

Trt::Trt(TrtPluginParams params) {
    mPluginFactory = new PluginFactory(params);
}

Trt::~Trt() {
    if(mPluginFactory != nullptr) {
        delete mPluginFactory;
        mPluginFactory = nullptr;
    }
}

void Trt::CreateEngine(const std::string& prototxt, 
                       const std::string& caffeModel,
                       const std::string& engineFile,
                       const std::vector<std::string>& outputBlobName,
                       const std::vector<std::vector<float>>& calibratorData,
                       int maxBatchSize,
                       int mode) {
    mRunMode = mode;
    if(!DeserializeEngine(engineFile)) {
        if(!BuildEngine(prototxt,caffeModel,engineFile,outputBlobName,calibratorData,maxBatchSize)) {
            spdlog::error("error: could not deserialize or build engine");
            return;
        }
    }
    spdlog::info("create execute context and malloc device memory...");
    InitEngine();
    // Notice: close profiler
    //mContext->setProfiler(mProfiler);
}

void Trt::CreateEngine(const std::string& onnxModelpath,
                       const std::string& engineFile,
                       int maxBatchSize) {
    if(!DeserializeEngine(engineFile)) {
        if(!BuildEngine(onnxModelpath,engineFile,maxBatchSize)) {
            spdlog::error("error: could not deserialize or build engine");
            return;
        }
    }
    spdlog::info("create execute context and malloc device memory...");
    InitEngine();
}

void Trt::Forward() {
    cudaEvent_t start,stop;
    float elapsedTime;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);
    cudaEventRecord(start, 0);
    mContext->execute(mBatchSize, &mBinding[0]);
    cudaEventRecord(stop, 0);
	cudaEventSynchronize(stop);
	cudaEventElapsedTime(&elapsedTime, start, stop);
    spdlog::info("net forward takes {} ms", elapsedTime);
}

void Trt::ForwardAsync(const cudaStream_t& stream) {
    mContext->enqueue(mBatchSize, &mBinding[0], stream, nullptr);
}

void Trt::PrintTime() {

}

void Trt::DataTransfer(std::vector<float>& data, int bindIndex, bool isHostToDevice) {
    if(isHostToDevice) {
        std::cout << "data size: " << data.size()*sizeof(float) << ", bindSize: " << mBindingSize[bindIndex] << std::endl;
        assert(data.size()*sizeof(float) == mBindingSize[bindIndex]);
        CUDA_CHECK(cudaMemcpy(mBinding[bindIndex], data.data(), mBindingSize[bindIndex], cudaMemcpyHostToDevice));
    } else {
        data.resize(mBindingSize[bindIndex]/sizeof(float));
        CUDA_CHECK(cudaMemcpy(data.data(), mBinding[bindIndex], mBindingSize[bindIndex], cudaMemcpyDeviceToHost));
    }
    std::cout << std::endl;
}

void Trt::DataTransferAsync(std::vector<float>& data, int bindIndex, bool isHostToDevice, cudaStream_t& stream) {
    if(isHostToDevice) {
        assert(data.size()*sizeof(float) == mBindingSize[bindIndex]);
        CUDA_CHECK(cudaMemcpyAsync(mBinding[bindIndex], data.data(), mBindingSize[bindIndex], cudaMemcpyHostToDevice, stream));
    } else {
        data.resize(mBindingSize[bindIndex]/sizeof(float));
        CUDA_CHECK(cudaMemcpyAsync(data.data(), mBinding[bindIndex], mBindingSize[bindIndex], cudaMemcpyDeviceToHost, stream));
    }
}

void Trt::CopyFromHostToDevice(const std::vector<float>& input, int bindIndex) {
    CUDA_CHECK(cudaMemcpy(mBinding[bindIndex], input.data(), mBindingSize[bindIndex], cudaMemcpyHostToDevice));
}

void Trt::CopyFromHostToDevice(const std::vector<float>& input, int bindIndex, const cudaStream_t& stream) {
    CUDA_CHECK(cudaMemcpyAsync(mBinding[bindIndex], input.data(), mBindingSize[bindIndex], cudaMemcpyHostToDevice, stream));
}

void Trt::CopyFromDeviceToHost(std::vector<float>& output, int bindIndex) {
    CUDA_CHECK(cudaMemcpy(output.data(), mBinding[bindIndex], mBindingSize[bindIndex], cudaMemcpyDeviceToHost));
}

void Trt::CopyFromDeviceToHost(std::vector<float>& output, int bindIndex, const cudaStream_t& stream) {
    CUDA_CHECK(cudaMemcpyAsync(output.data(), mBinding[bindIndex], mBindingSize[bindIndex], cudaMemcpyDeviceToHost, stream));
}

int Trt::GetMaxBatchSize() {
    return mBatchSize;
}

void* Trt::GetBindingPtr(int bindIndex) const {
    return mBinding[bindIndex];
}

size_t Trt::GetBindingSize(int bindIndex) const {
    return mBindingSize[bindIndex];
}

nvinfer1::Dims Trt::GetBindingDims(int bindIndex) const {
    return mBindingDims[bindIndex];
}

nvinfer1::DataType Trt::GetBindingDataType(int bindIndex) const {
    return mBindingDataType[bindIndex];
}

void Trt::SaveEngine(const std::string& fileName) {
    if(fileName == "") {
        spdlog::warn("empty engine file name, skip save");
        return;
    }
    if(mEngine != nullptr) {
        spdlog::info("save engine to {}...",fileName);
        nvinfer1::IHostMemory* data = mEngine->serialize();
        std::ofstream file;
        file.open(fileName,std::ios::binary | std::ios::out);
        if(!file.is_open()) {
            spdlog::error("read create engine file {} failed",fileName);
            return;
        }
        file.write((const char*)data->data(), data->size());
        file.close();
        data->destroy();
    } else {
        spdlog::error("engine is empty, save engine failed");
    }
}

bool Trt::DeserializeEngine(const std::string& engineFile) {
    std::ifstream in(engineFile.c_str(), std::ifstream::binary);
    if(in.is_open()) {
        spdlog::info("deserialize engine from {}",engineFile);
        auto const start_pos = in.tellg();
        in.ignore(std::numeric_limits<std::streamsize>::max());
        size_t bufCount = in.gcount();
        in.seekg(start_pos);
        std::unique_ptr<char[]> engineBuf(new char[bufCount]);
        in.read(engineBuf.get(), bufCount);
        initLibNvInferPlugins(&mLogger, "");
        mRuntime = nvinfer1::createInferRuntime(mLogger);
        mEngine = mRuntime->deserializeCudaEngine((void*)engineBuf.get(), bufCount, nullptr);
        mBatchSize = mEngine->getMaxBatchSize();
        spdlog::info("max batch size of deserialized engine: {}",mEngine->getMaxBatchSize());
        return true;
    }
    return false;
}

bool Trt::BuildEngine(const std::string& prototxt, 
                        const std::string& caffeModel,
                        const std::string& engineFile,
                        const std::vector<std::string>& outputBlobName,
                        const std::vector<std::vector<float>>& calibratorData,
                        int maxBatchSize) {
        mBatchSize = maxBatchSize;
        spdlog::info("build caffe engine with {} and {}", prototxt, caffeModel);
        nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(mLogger);
        assert(builder != nullptr);
        nvinfer1::INetworkDefinition* network = builder->createNetwork();
        assert(network != nullptr);
        nvcaffeparser1::ICaffeParser* parser = nvcaffeparser1::createCaffeParser();
        if(mPluginFactory != nullptr) {
            parser->setPluginFactoryV2(mPluginFactory);
        }
        // Notice: change here to costom data type
        const nvcaffeparser1::IBlobNameToTensor* blobNameToTensor = parser->parse(prototxt.c_str(),caffeModel.c_str(),*network,nvinfer1::DataType::kFLOAT);
        for(auto& s : outputBlobName) {
            network->markOutput(*blobNameToTensor->find(s.c_str()));
        }
        spdlog::info("Number of network layers: {}",network->getNbLayers());
        spdlog::info("Number of input: ", network->getNbInputs());
        std::cout << "Input layer: " << std::endl;
        for(int i = 0; i < network->getNbInputs(); i++) {
            std::cout << network->getInput(i)->getName() << " : ";
            Dims dims = network->getInput(i)->getDimensions();
            for(int j = 0; j < dims.nbDims; j++) {
                std::cout << dims.d[j] << "x"; 
            }
            std::cout << "\b "  << std::endl;
        }
        spdlog::info("Number of output: {}",network->getNbOutputs());
        std::cout << "Output layer: " << std::endl;
        for(int i = 0; i < network->getNbOutputs(); i++) {
            std::cout << network->getOutput(i)->getName() << " : ";
            Dims dims = network->getOutput(i)->getDimensions();
            for(int j = 0; j < dims.nbDims; j++) {
                std::cout << dims.d[j] << "x"; 
            }
            std::cout << "\b " << std::endl;
        }
        spdlog::info("parse network done");
        Int8EntropyCalibrator* calibrator = nullptr;
        if (mRunMode == 2)
        {
            spdlog::info("set int8 inference mode");
            if (!builder->platformHasFastInt8())
                spdlog::warn("Warning: current platform doesn't support int8 inference");
            builder->setInt8Mode(true);
            
            if (calibratorData.size() > 0 ){
                auto endPos= prototxt.find_last_of(".");
                auto beginPos= prototxt.find_last_of('/') + 1;
                if(prototxt.find("/") == std::string::npos) {
                    beginPos = 0;
                }
                std::string calibratorName = prototxt.substr(beginPos,endPos - beginPos);
                std::cout << "create calibrator,Named:" << calibratorName << std::endl;
                calibrator = new Int8EntropyCalibrator(maxBatchSize,calibratorData,calibratorName,false);
            }
            builder->setInt8Calibrator(calibrator);
        }
        
        if (mRunMode == 1)
        {
            spdlog::info("setFp16Mode");
            if (!builder->platformHasFastFp16()) {
                spdlog::warn("the platform do not has fast for fp16");
            }
            builder->setFp16Mode(true);
        }
        builder->setMaxBatchSize(mBatchSize);
        builder->setMaxWorkspaceSize(10 << 20); // Warning: here might have bug
        spdlog::info("fp16 support: {}",builder->platformHasFastFp16 ());
        spdlog::info("int8 support: {}",builder->platformHasFastInt8 ());
        spdlog::info("Max batchsize: {}",builder->getMaxBatchSize());
        spdlog::info("Max workspace size: {}",builder->getMaxWorkspaceSize());
        spdlog::info("Number of DLA core: {}",builder->getNbDLACores());
        spdlog::info("Max DLA batchsize: {}",builder->getMaxDLABatchSize());
        spdlog::info("Current use DLA core: {}",builder->getDLACore());
        spdlog::info("Half2 mode: {}",builder->getHalf2Mode());
        spdlog::info("INT8 mode: {}",builder->getInt8Mode());
        spdlog::info("FP16 mode: {}",builder->getFp16Mode());
        spdlog::info("build engine...");
        mEngine = builder -> buildCudaEngine(*network);
        assert(mEngine != nullptr);
        spdlog::info("serialize engine to {}", engineFile);
        SaveEngine(engineFile);
        
        builder->destroy();
        network->destroy();
        parser->destroy();
        if(calibrator){
            delete calibrator;
            calibrator = nullptr;
        }
        return true;
}

bool Trt::BuildEngine(const std::string& onnxModelpath,
                      const std::string& engineFile,
                      int maxBatchSize) {
    spdlog::warn("The ONNX Parser shipped with TensorRT 5.1.x supports ONNX IR (Intermediate Representation) version 0.0.3, opset version 9");
    mBatchSize = maxBatchSize;
    spdlog::info("build onnx engine from {}...",onnxModelpath);
    nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(mLogger);
    assert(builder != nullptr);
    nvinfer1::INetworkDefinition* network = builder->createNetwork();
    assert(network != nullptr);
    nvonnxparser::IParser* parser = nvonnxparser::createParser(*network, mLogger);
    if(!parser->parseFromFile(onnxModelpath.c_str(), static_cast<int>(ILogger::Severity::kWARNING))) {
        spdlog::error("error: could not parse onnx engine");
        return false;
    }
    mEngine = builder->buildCudaEngine(*network);
    assert(mEngine != nullptr);
    spdlog::info("serialize engine to {}", engineFile);
    SaveEngine(engineFile);

    builder->destroy();
    network->destroy();
    parser->destroy();
    return true;
}

void Trt::InitEngine() {
    spdlog::info("init engine...");
    mContext = mEngine->createExecutionContext();
    assert(mContext != nullptr);

    spdlog::info("malloc device memory");
    int nbBindings = mEngine->getNbBindings();
    std::cout << "nbBingdings: " << nbBindings << std::endl;
    mBinding.resize(nbBindings);
    mBindingSize.resize(nbBindings);
    mBindingName.resize(nbBindings);
    mBindingDims.resize(nbBindings);
    mBindingDataType.resize(nbBindings);
    for(int i=0; i< nbBindings; i++) {
        nvinfer1::Dims dims = mEngine->getBindingDimensions(i);
        nvinfer1::DataType dtype = mEngine->getBindingDataType(i);
        const char* name = mEngine->getBindingName(i);
        int64_t totalSize = volume(dims) * mBatchSize * getElementSize(dtype);
        mBindingSize[i] = totalSize;
        mBindingName[i] = name;
        mBindingDims[i] = dims;
        mBindingDataType[i] = dtype;
        if(mEngine->bindingIsInput(i)) {
            spdlog::info("input: ");
        } else {
            spdlog::info("output: ");
        }
        spdlog::info("binding bindIndex: {}, name: {}, size in byte: {}",i,name,totalSize);
        spdlog::info("binding dims with {} dimemsion",dims.nbDims);
        for(int j=0;j<dims.nbDims;j++) {
            std::cout << dims.d[j] << " x ";
        }
        std::cout << "\b\b  "<< std::endl;
        mBinding[i] = safeCudaMalloc(totalSize);
        if(mEngine->bindingIsInput(i)) {
            mInputSize++;
        }
    }
}