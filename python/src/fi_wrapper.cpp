/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <pybind11/pybind11.h>

#include "frequent_items_sketch.hpp"

namespace py = pybind11;

template<typename T>
void bind_fi_sketch(py::module &m, const char* name) {
  using namespace datasketches;

  py::class_<frequent_items_sketch<T>>(m, name)
    .def(py::init<uint8_t>(), py::arg("lg_max_k"))
    .def("__str__", &frequent_items_sketch<T>::to_string, py::arg("print_items")=false,
         "Produces a string summary of the sketch")
    .def("to_string", &frequent_items_sketch<T>::to_string, py::arg("print_items")=false,
         "Produces a string summary of the sketch")
    .def("update", (void (frequent_items_sketch<T>::*)(const T&, uint64_t)) &frequent_items_sketch<T>::update, py::arg("item"), py::arg("weight")=1,
         "Updates the sketch with the given string and, optionally, a weight")
    .def("merge", (void (frequent_items_sketch<T>::*)(const frequent_items_sketch<T>&)) &frequent_items_sketch<T>::merge,
         "Merges the given sketch into this one")
    .def("is_empty", &frequent_items_sketch<T>::is_empty,
         "Returns True if the sketch is empty, otherwise False")
    .def("get_num_active_items", &frequent_items_sketch<T>::get_num_active_items,
         "Returns the number of active items in the sketch")
    .def("get_total_weight", &frequent_items_sketch<T>::get_total_weight,
         "Returns the sum of the weights (frequencies) in the stream seen so far by the sketch")
    .def("get_estimate", &frequent_items_sketch<T>::get_estimate, py::arg("item"),
         "Returns the estimate of the weight (frequency) of the given item.\n"
         "Note: The true frequency of a item would be the sum of the counts as a result of the "
         "two update functions.")
    .def("get_lower_bound", &frequent_items_sketch<T>::get_lower_bound, py::arg("item"),
         "Returns the guaranteed lower bound weight (frequency) of the given item.")
    .def("get_upper_bound", &frequent_items_sketch<T>::get_upper_bound, py::arg("item"),
         "Returns the guaranteed upper bound weight (frequency) of the given item.")
    .def("get_sketch_epsilon", (double (frequent_items_sketch<T>::*)(void) const) &frequent_items_sketch<T>::get_epsilon,
         "Returns the epsilon value used by the sketch to compute error")
    .def(
        "get_frequent_items",
        [](const frequent_items_sketch<T>& sk, frequent_items_error_type err_type, uint64_t threshold) {
          if (threshold == 0) threshold = sk.get_maximum_error();
          py::list list;
          auto rows = sk.get_frequent_items(err_type, threshold);
          for (auto row: rows) {
            list.append(py::make_tuple(
                row.get_item(),
                row.get_estimate(),
                row.get_lower_bound(),
                row.get_upper_bound())
            );
          }
          return list;
        },
        py::arg("err_type"), py::arg("threshold")=0
    )
    .def_static(
        "get_epsilon_for_lg_size",
        [](uint8_t lg_max_map_size) { return frequent_items_sketch<T>::get_epsilon(lg_max_map_size); },
        py::arg("lg_max_map_size"),
        "Returns the epsilon value used to compute a priori error for a given log2(max_map_size)"
    )
    .def_static(
        "get_apriori_error",
        &frequent_items_sketch<T>::get_apriori_error,
        py::arg("lg_max_map_size"), py::arg("estimated_total_weight"),
        "Returns the estimated a priori error given the max_map_size for the sketch and the estimated_total_stream_weight."
    )
    .def(
        "get_serialized_size_bytes",
        [](const frequent_items_sketch<T>& sk) { return sk.get_serialized_size_bytes(); },
        "Computes the size needed to serialize the current state of the sketch. This can be expensive since every item needs to be looked at."
    )
    .def(
        "serialize",
        [](const frequent_items_sketch<T>& sk) {
          auto bytes = sk.serialize();
          return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        },
        "Serializes the sketch into a bytes object"
    )
    .def_static(
        "deserialize",
        [](const std::string& bytes) { return frequent_items_sketch<T>::deserialize(bytes.data(), bytes.size()); },
        py::arg("bytes"),
        "Reads a bytes object and returns the corresponding frequent_strings_sketch"
    );
}

void init_fi(py::module &m) {
  using namespace datasketches;

  py::enum_<frequent_items_error_type>(m, "frequent_items_error_type")
    .value("NO_FALSE_POSITIVES", NO_FALSE_POSITIVES)
    .value("NO_FALSE_NEGATIVES", NO_FALSE_NEGATIVES)
    .export_values();

  bind_fi_sketch<std::string>(m, "frequent_strings_sketch");
}
