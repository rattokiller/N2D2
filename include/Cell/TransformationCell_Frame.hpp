/*
    (C) Copyright 2015 CEA LIST. All Rights Reserved.
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

#ifndef N2D2_TRANSFORMATIONCELL_FRAME_H
#define N2D2_TRANSFORMATIONCELL_FRAME_H

#include <tuple>
#include <unordered_map>
#include <vector>

#include "Cell_Frame.hpp"
#include "DeepNet.hpp"
#include "TransformationCell.hpp"

namespace N2D2 {
class TransformationCell_Frame : public virtual TransformationCell,
                                 public Cell_Frame<Float_T> {
public:
    TransformationCell_Frame(const DeepNet& deepNet, const std::string& name,
                             unsigned int nbOutputs,
                             const std::shared_ptr
                             <Transformation>& transformation);
    static std::shared_ptr<TransformationCell>
    create(const DeepNet& deepNet, const std::string& name,
           unsigned int nbOutputs,
           const std::shared_ptr<Transformation>& transformation)
    {
        return std::make_shared
            <TransformationCell_Frame>(deepNet, name, nbOutputs, transformation);
    }

    virtual void setOutputTarget(const Tensor<int>& /*targets*/) {};
    virtual void setOutputTargets(const BaseTensor& /*targets*/) {};
    virtual void propagate(bool inference = false);
    virtual void backPropagate() {};
    virtual void update() {};
    void checkGradient(double /*epsilon*/ = 1.0e-4,
                       double /*maxError*/ = 1.0e-6) {};
    virtual ~TransformationCell_Frame() {};

private:
    static Registrar<TransformationCell> mRegistrar;
};
}

#endif // N2D2_TRANSFORMATIONCELL_FRAME_H
