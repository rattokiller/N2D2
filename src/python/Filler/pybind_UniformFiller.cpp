/*
    (C) Copyright 2021 CEA LIST. All Rights Reserved.
    Contributor(s): Olivier BICHLER (olivier.bichler@cea.fr)
                    Cyril MOINEAU (cyril.moineau@cea.fr)
                    Johannes THIELE (johannes.thiele@cea.fr)

    This software is governed by the CeCILL-C license under French law and
    abiding by the rules of distribution of free software.  You can  use,
    modify and/ or redistribute the software under the terms of the CeCILL-C
    license as circulated by CEA, CNRS and INRIA at the following URL
    "http://www.cecill.info".

    As a counterpart to the access to the source code and  rights to copy,
    modify and redistribute granted by the license, users are provided only
    with a limited warranty  and the software's author,  the holder of the
    economic rights,  and the successive licensors  have only  limited
    liability.

    The fact that you are presently reading this means that you have had
    knowledge of the CeCILL-C license and that you accept its terms.
*/

#ifdef PYBIND
#include "Filler/UniformFiller.hpp"


#include <pybind11/pybind11.h>

namespace py = pybind11;


namespace N2D2 {
template<typename T>
void declare_UniformFiller(py::module &m, const std::string& typeStr) {
    const std::string pyClassName("UniformFiller_" + typeStr);

    py::class_<UniformFiller<T>, std::shared_ptr<UniformFiller<T>>, Filler> filler(
            m, pyClassName.c_str(), py::multiple_inheritance()
    );

    filler
    .def(py::init<T, T>(), 
        py::arg("min"), 
        py::arg("max"))
    .def("getMax", &UniformFiller<T>::getMax)
    .def("getMin", &UniformFiller<T>::getMin)
    .def("getDataType", [typeStr](UniformFiller<T>){return typeStr;})
    ;
}

void init_UniformFiller(py::module &m) {
    declare_UniformFiller<float>(m, "float");
    declare_UniformFiller<double>(m, "double");
}
}

#endif
