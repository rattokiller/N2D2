/*
    (C) Copyright 2019 CEA LIST. All Rights Reserved.
    Contributor(s): Olivier BICHLER (olivier.bichler@cea.fr)

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

#ifndef N2D2_SCALING_CELL_FRAME_CUDA_H
#define N2D2_SCALING_CELL_FRAME_CUDA_H

#include <memory>
#include <string>

#include "Cell_Frame_CUDA.hpp"
#include "Scaling.hpp"
#include "ScalingCell.hpp"

namespace N2D2 {

class DeepNet;

template<class T>
class ScalingCell_Frame_CUDA : public virtual ScalingCell, public Cell_Frame_CUDA<T> {
public:
    using Cell_Frame_CUDA<T>::mInputs;
    using Cell_Frame_CUDA<T>::mOutputs;
    using Cell_Frame_CUDA<T>::mDiffInputs;
    using Cell_Frame_CUDA<T>::mDiffOutputs;

    
    ScalingCell_Frame_CUDA(const DeepNet& deepNet, const std::string& name,
                           unsigned int nbOutputs, Scaling scaling);
    virtual ~ScalingCell_Frame_CUDA() = default;

    static std::shared_ptr<ScalingCell> create(const DeepNet& deepNet, const std::string& name,
                                               unsigned int nbOutputs, Scaling scaling)
    {
        return std::make_shared<ScalingCell_Frame_CUDA>(deepNet, name, nbOutputs, scaling);
    }

    virtual void initialize();
    virtual void propagate(bool inference = false);
    virtual void backPropagate();
    virtual void update();
    void checkGradient(double epsilon = 1.0e-4, double maxError = 1.0e-6);
    
    std::pair<double, double> getOutputsRange() const;
};
}

#endif // N2D2_SCALING_CELL_FRAME_CUDA_H
