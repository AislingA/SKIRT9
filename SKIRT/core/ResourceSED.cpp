/*//////////////////////////////////////////////////////////////////
////     The SKIRT project -- advanced radiative transfer       ////
////       © Astronomical Observatory, Ghent University         ////
///////////////////////////////////////////////////////////////// */

#include "ResourceSED.hpp"
#include "Random.hpp"
#include "WavelengthRangeInterface.hpp"

//////////////////////////////////////////////////////////////////////

void ResourceSED::setupSelfBefore()
{
    SED::setupSelfBefore();

    _table.open(this, resourceName(), "lambda(m)", "Llambda(W/m)");
    _Ltot = _table.cdf(_lambdav, _pv, _Pv, interface<WavelengthRangeInterface>()->wavelengthRange());
}

//////////////////////////////////////////////////////////////////////

double ResourceSED::specificLuminosity(double wavelength) const
{
    return _table[wavelength] / _Ltot;
}

//////////////////////////////////////////////////////////////////////

double ResourceSED::integratedLuminosity(const Range& wavelengthRange) const
{
    Array lambdav, pv, Pv;  // the contents of these arrays is not used, so this could be optimized if needed
    return _table.cdf(lambdav, pv, Pv, wavelengthRange) / _Ltot;
}

//////////////////////////////////////////////////////////////////////

double ResourceSED::generateWavelength() const
{
    return random()->cdfLogLog(_lambdav, _pv, _Pv);
}

//////////////////////////////////////////////////////////////////////
