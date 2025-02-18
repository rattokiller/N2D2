/*
    (C) Copyright 2016 CEA LIST. All Rights Reserved.
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

#ifndef N2D2_WALLISFILTERTRANSFORMATION_H
#define N2D2_WALLISFILTERTRANSFORMATION_H

#include "Transformation/Transformation.hpp"

namespace N2D2 {
class WallisFilterTransformation : public Transformation {
public:
    using Transformation::apply;

    static const char* Type;

    WallisFilterTransformation(unsigned int size,
                               double mean = 0.0,
                               double stdDev = 1.0);
    WallisFilterTransformation(const WallisFilterTransformation& trans);
    const char* getType() const
    {
        return Type;
    };
    void apply(cv::Mat& frame,
               cv::Mat& /*labels*/,
               std::vector<std::shared_ptr<ROI> >& /*labelsROI*/,
               int /*id*/ = -1);
    std::shared_ptr<WallisFilterTransformation> clone() const
    {
        return std::shared_ptr<WallisFilterTransformation>(doClone());
    }
    std::pair<unsigned int, unsigned int>
    getOutputsSize(unsigned int width, unsigned int height) const
    {
        return std::make_pair(width, height);
    };
    int getOutputsDepth(int /*depth*/) const
    {
        return CV_32F;
    };
    virtual ~WallisFilterTransformation();

private:
    virtual WallisFilterTransformation* doClone() const
    {
        return new WallisFilterTransformation(*this);
    }
    cv::Mat wallisFilter(cv::Mat& mat) const;

    const unsigned int mSize;
    const double mMean;
    const double mStdDev;

    Parameter<bool> mPerChannel;
};
}

#endif // N2D2_WALLISFILTERTRANSFORMATION_H
