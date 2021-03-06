// Copyright 2016 Husky Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "husky/base/log.hpp"
#include "husky/base/serialization.hpp"
#include "husky/core/executor.hpp"
#include "husky/core/objlist.hpp"
#include "husky/core/zmq_helpers.hpp"
#include "husky/lib/aggregator_factory.hpp"
#include "husky/lib/ml/data_loader.hpp"
#include "husky/lib/ml/feature_label.hpp"
#include "husky/lib/ml/linear_regression.hpp"
#include "husky/lib/ml/scaler.hpp"
#include "husky/lib/ml/sgd.hpp"

#include "backend/itc.hpp"
#include "backend/pythonconnector.hpp"

namespace husky {

class PythonConnector;
class ITCWorker;
class ITCDaemon;
class PythonSocket;
class ThreadConnector;
class Operation;

using base::BinStream;

using husky::lib::ml::ParameterBucket;

class PyHuskyLinearR {
   public:
    static void init_py_handlers();
    static void init_cpp_handlers();
    static void init_daemon_handlers();

   protected:
    // thread handlers
    static void LinearR_load_pyhlist_handler(PythonSocket& python_socket, ITCWorker& daemon_socket);

    // cpp handlers
    static void LinearR_init_handler(const Operation& op, PythonSocket& python_socket, ITCWorker& daemon_socket);
    static void LinearR_load_hdfs_handler(const Operation& op, PythonSocket& python_socket, ITCWorker& daemon_socket);
    static void LinearR_train_handler(const Operation& op, PythonSocket& python_socket, ITCWorker& daemon_socket);

    // daemon handlers
    static void daemon_train_handler(ITCDaemon&, BinStream&);
};  // class PyHuskyLinearR

class ModelBase {
   public:
    virtual ~ModelBase() {}
};

template <class A>
class Model : public ModelBase {
   public:
    explicit Model(A* model) { m = model; }
    ~Model() {}
    A* get_model() { return m; }

   private:
    A* m;
};

extern thread_local std::map<std::string, std::shared_ptr<ModelBase>> local_SGD_LinearR_model;

template <bool is_sparse>
void Linear_create_model_from_url(std::string name, std::string url, husky::lib::ml::DataFormat data_format) {
    husky::base::log_msg("create model name: " + name);

    typedef husky::lib::ml::LabeledPointHObj<double, double, is_sparse> LabeledPointHObj;
    auto& load_list = husky::ObjListStore::create_objlist<LabeledPointHObj>(name);

    // load data
    int num_features = husky::lib::ml::load_data(url, load_list, data_format);

    assert(num_features > 0);
    local_SGD_LinearR_model[name] =
        std::make_shared<Model<husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>>>(
            new husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>(num_features));
    std::static_pointer_cast<
        Model<husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>>>(
        local_SGD_LinearR_model.at(name))
        ->get_model()
        ->report_per_round = true;
}

template <bool is_sparse>
void Linear_create_model_from_pyhuskylist(std::string name, PythonSocket& python_socket, ITCWorker& daemon_socket) {
    husky::base::log_msg("create model name: " + name);

    typedef husky::lib::ml::LabeledPointHObj<double, double, is_sparse> LabeledPointHObj;
    auto& load_list = husky::ObjListStore::create_objlist<LabeledPointHObj>(name);

    int n_sample = std::stoi(zmq_recv_string(python_socket.pipe_from_python));

    husky::lib::Aggregator<int> n_feature_agg(0, [](int& a, const int& b) { a = std::max(a, b); });
    auto& ac = husky::lib::AggregatorFactory::get_channel();

    int keep_n_feature = 0;

    for (int i = 0; i < n_sample; i++) {
        int n_feature = std::stoi(zmq_recv_string(python_socket.pipe_from_python));
        LabeledPointHObj this_obj(n_feature);
        for (int j = 0; j < n_feature; j++) {
            double X_elem = std::stod(zmq_recv_string(python_socket.pipe_from_python));
            this_obj.x.set(j, X_elem);
        }
        keep_n_feature = n_feature;
        n_feature_agg.update(n_feature);
        double y = std::stod(zmq_recv_string(python_socket.pipe_from_python));
        this_obj.y = y;
        load_list.add_object(this_obj);
    }

    int num_features = keep_n_feature;

    list_execute(load_list, [&](LabeledPointHObj& this_obj) {
        if (this_obj.x.get_feature_num() != num_features) {
            this_obj.x.resize(num_features);
        }
    });

    assert(num_features > 0);
    local_SGD_LinearR_model[name] =
        std::make_shared<Model<husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>>>(
            new husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>(num_features));
    std::static_pointer_cast<
        Model<husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>>>(
        local_SGD_LinearR_model.at(name))
        ->get_model()
        ->report_per_round = true;
}

template <bool is_sparse>
void Linear_train_model(std::string name, double alpha, int num_iter) {
    husky::base::log_msg("start training name: " + name);

    typedef husky::lib::ml::LabeledPointHObj<double, double, is_sparse> LabeledPointHObj;
    auto& train_list = husky::ObjListStore::get_objlist<LabeledPointHObj>(name);

    int n_feature = std::static_pointer_cast<
                        Model<husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>>>(
                        local_SGD_LinearR_model.at(name))
                        ->get_model()
                        ->get_num_feature();

    husky::lib::ml::LinearScaler<double, double, is_sparse> linscaler(n_feature);
    linscaler.fit_transform(train_list);

    // train
    std::static_pointer_cast<
        Model<husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>>>(
        local_SGD_LinearR_model.at(name))
        ->get_model()
        ->template train<husky::lib::ml::SGD>(train_list, num_iter, alpha);
}

template <bool is_sparse>
BinStream Linear_get_params(std::string name) {
    int n_param = std::static_pointer_cast<
                      Model<husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>>>(
                      local_SGD_LinearR_model.at(name))
                      ->get_model()
                      ->get_num_param();
    BinStream result;
    result << n_param;
    assert(n_param > 0);
    auto params_vec = std::static_pointer_cast<
                          Model<husky::lib::ml::LinearRegression<double, double, is_sparse, ParameterBucket<double>>>>(
                          local_SGD_LinearR_model.at(name))
                          ->get_model()
                          ->get_param();
    int n = params_vec.get_num_param();
    for (int i = 0; i < n; i++) {
        result << params_vec.param_at(i);
    }

    return result;
}

}  // namespace husky
