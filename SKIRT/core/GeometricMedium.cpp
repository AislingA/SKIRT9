/*//////////////////////////////////////////////////////////////////
////     The SKIRT project -- advanced radiative transfer       ////
////       © Astronomical Observatory, Ghent University         ////
///////////////////////////////////////////////////////////////// */

#include "GeometricMedium.hpp"

////////////////////////////////////////////////////////////////////

void GeometricMedium::setupSelfAfter()
{
    Medium::setupSelfAfter();

    // determine normalization
    std::tie(_number, _mass) = normalization()->numberAndMass(geometry(), materialMix());
}

////////////////////////////////////////////////////////////////////

int GeometricMedium::dimension() const
{
    int velocityDimension = 1;
    if (velocityZ()) velocityDimension = 2;
    if (velocityX() || velocityY()) velocityDimension = 3;
    return max(geometry()->dimension(), velocityDimension);
}

////////////////////////////////////////////////////////////////////

const MaterialMix*GeometricMedium::mix(Position /*bfr*/) const
{
    return materialMix();
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::numberDensity(Position bfr) const
{
    return _number * geometry()->density(bfr);
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::numberColumnDensityX() const
{
    return _number * geometry()->SigmaX();
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::numberColumnDensityY() const
{
    return _number * geometry()->SigmaY();
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::numberColumnDensityZ() const
{
    return _number * geometry()->SigmaZ();
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::number() const
{
    return _number;
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::massDensity(Position bfr) const
{
    return _mass * geometry()->density(bfr);
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::massColumnDensityX() const
{
    return _mass * geometry()->SigmaX();
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::massColumnDensityY() const
{
    return _mass * geometry()->SigmaY();
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::massColumnDensityZ() const
{
    return _mass * geometry()->SigmaZ();
}

////////////////////////////////////////////////////////////////////

double GeometricMedium::mass() const
{
    return _mass;
}

////////////////////////////////////////////////////////////////////

Position GeometricMedium::generatePosition() const
{
    return geometry()->generatePosition();
}

////////////////////////////////////////////////////////////////////
