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

#include "Generator/StripeRemoveTransformationGenerator.hpp"

N2D2::Registrar<N2D2::TransformationGenerator>
N2D2::StripeRemoveTransformationGenerator::mRegistrar(
    "StripeRemoveTransformation", N2D2::StripeRemoveTransformationGenerator::generate);

std::shared_ptr<N2D2::StripeRemoveTransformation>
N2D2::StripeRemoveTransformationGenerator::generate(IniParser& iniConfig,
                                               const std::string& section)
{
    if (!iniConfig.currentSection(section))
        throw std::runtime_error("Missing [" + section + "] section.");

    const int axis = iniConfig.getProperty<int>("Axis");
    const unsigned int offset = iniConfig.getProperty<unsigned int>("Offset");
    const unsigned int length = iniConfig.getProperty<unsigned int>("Length");

    std::shared_ptr<StripeRemoveTransformation> trans = std::make_shared
        <StripeRemoveTransformation>(axis, offset, length);
    trans->setParameters(iniConfig.getSection(section, true));
    return trans;
}
