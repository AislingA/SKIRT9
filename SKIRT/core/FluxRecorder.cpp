/*//////////////////////////////////////////////////////////////////
////     The SKIRT project -- advanced radiative transfer       ////
////       © Astronomical Observatory, Ghent University         ////
///////////////////////////////////////////////////////////////// */

#include "FluxRecorder.hpp"
#include "FITSInOut.hpp"
#include "LockFree.hpp"
#include "Log.hpp"
#include "PhotonPacket.hpp"
#include "ProcessManager.hpp"
#include "StringUtils.hpp"
#include "TextOutFile.hpp"
#include "Units.hpp"
#include "WavelengthGrid.hpp"

////////////////////////////////////////////////////////////////////

namespace
{
    // indices for detector arrays that need calibration
    enum { Total=0, Transparent, PrimaryDirect, PrimaryScattered, SecondaryDirect, SecondaryScattered,
           TotalQ, TotalU, TotalV, PrimaryScatteredLevel };

    // number of detector arrays for statistics (which do not need calibration)
    const int numStatMoments = 4;
}

////////////////////////////////////////////////////////////////////

// data structures to queue and combine all contributions from a photon packet history to a statistics bin;
// we assume that all detections for a given history are handled inside the same execution thread
// and that histories (within a particular thread) are handled one after the other (not interleaved)

namespace
{
    // structure to remember a single contribution
    class Contribution
    {
    public:
        Contribution(int ell, int l, double w) : _ell(ell), _l(l), _w(w) { }
        bool operator<(const Contribution& c) const { return std::tie(_ell, _l) < std::tie(c._ell, c._l); }
        int ell() const { return _ell; }
        int l() const { return _l; }
        double w() const { return _w; }
    private:
        int _ell{0};     // wavelength index
        int _l{0};       // pixel index (relevant only for IFUs)
        double _w{0};    // contribution
    };
}

namespace FluxRecorder_Impl
{
    // structure to remember a list of contributions for a given photon packet history
    class ContributionList
    {
    public:
        bool hasHistoryIndex(size_t historyIndex) const { return _historyIndex == historyIndex; }
        void addContribution(int ell, int l, double w) { _contributions.emplace_back(ell, l, w); }
        void reset(size_t historyIndex = 0) { _historyIndex = historyIndex, _contributions.clear(); }
        void sort() { std::sort(_contributions.begin(), _contributions.end()); }
        const vector<Contribution>& contributions() const { return _contributions; }
    private:
        size_t _historyIndex{0};
        vector<Contribution> _contributions;
    };
}

////////////////////////////////////////////////////////////////////

FluxRecorder::FluxRecorder()
{
}

////////////////////////////////////////////////////////////////////

void FluxRecorder::setSimulationInfo(string instrumentName, const WavelengthGrid* lambdagrid,
                                     bool hasMedium, bool hasMediumEmission)
{
    _instrumentName = instrumentName;
    _lambdagrid = lambdagrid;
    _hasMedium = hasMedium;
    _hasMediumEmission = hasMediumEmission;
}

////////////////////////////////////////////////////////////////////

void FluxRecorder::setUserFlags(bool recordComponents, int numScatteringLevels,
                                bool recordPolarization, bool recordStatistics)
{
    _recordComponents = recordComponents;
    _numScatteringLevels = numScatteringLevels;
    _recordPolarization = recordPolarization;
    _recordStatistics = recordStatistics;
}

////////////////////////////////////////////////////////////////////

void FluxRecorder::includeFluxDensity(double distance)
{
    _includeFluxDensity = true;
    _distance = distance;
}

////////////////////////////////////////////////////////////////////

void FluxRecorder::includeSurfaceBrightness(double distance, int numPixelsX, int numPixelsY,
                                            double pixelSizeX, double pixelSizeY, double centerX, double centerY)
{
    _includeSurfaceBrightness = true;
    _distance = distance;
    _numPixelsX = numPixelsX;
    _numPixelsY = numPixelsY;
    _pixelSizeX = pixelSizeX;
    _pixelSizeY = pixelSizeY;
    _centerX = centerX;
    _centerY = centerY;
}

////////////////////////////////////////////////////////////////////

void FluxRecorder::finalizeConfiguration()
{
    // get array lengths
    _numPixelsInFrame = _numPixelsX * _numPixelsY;  // convert to size_t before calculating lenIFU
    size_t lenSED = _includeFluxDensity ? _lambdagrid->numWavelengths() : 0;
    size_t lenIFU = _includeSurfaceBrightness ? _numPixelsInFrame * _lambdagrid->numWavelengths() : 0;

    // do not try to record components if there is no medium
    _recordTotalOnly = !_recordComponents || !_hasMedium;

    // allocate the appropriate number of flux detector arrays
    _sed.resize(PrimaryScatteredLevel + _numScatteringLevels);
    _ifu.resize(PrimaryScatteredLevel + _numScatteringLevels);

    // resize the flux detector arrays according to the configuration
    if (_recordTotalOnly)
    {
        _sed[Total].resize(lenSED);  _ifu[Total].resize(lenIFU);
    }
    else
    {
        _sed[Transparent].resize(lenSED);       _ifu[Transparent].resize(lenIFU);
        _sed[PrimaryDirect].resize(lenSED);     _ifu[PrimaryDirect].resize(lenIFU);
        _sed[PrimaryScattered].resize(lenSED);  _ifu[PrimaryScattered].resize(lenIFU);

        for (int i=0; i!=_numScatteringLevels; ++i)
        {
            _sed[PrimaryScatteredLevel+i].resize(lenSED);  _ifu[PrimaryScatteredLevel+i].resize(lenIFU);
        }
        if (_hasMediumEmission)
        {
            _sed[SecondaryDirect].resize(lenSED);     _ifu[SecondaryDirect].resize(lenIFU);
            _sed[SecondaryScattered].resize(lenSED);  _ifu[SecondaryScattered].resize(lenIFU);
        }
    }
    if (_recordPolarization)
    {
        _sed[TotalQ].resize(lenSED);  _ifu[TotalQ].resize(lenIFU);
        _sed[TotalU].resize(lenSED);  _ifu[TotalU].resize(lenIFU);
        _sed[TotalV].resize(lenSED);  _ifu[TotalV].resize(lenIFU);
    }

    // allocate and resize the statistics detector arrays
    if (_recordStatistics)
    {
        _wsed.resize(numStatMoments);  _wifu.resize(numStatMoments);
        for (auto& array : _wsed) array.resize(lenSED);
        for (auto& array : _wifu) array.resize(lenIFU);
    }

    // calculate and log allocated memory size
    size_t allocatedSize = 0;
    for (const auto& array : _sed) allocatedSize += array.size();
    for (const auto& array : _ifu) allocatedSize += array.size();
    for (const auto& array : _wsed) allocatedSize += array.size();
    for (const auto& array : _wifu) allocatedSize += array.size();
    _lambdagrid->find<Log>()->info("Instrument " + _instrumentName + " allocated " +
                                   StringUtils::toMemSizeString(allocatedSize) + " of memory");
}

////////////////////////////////////////////////////////////////////

void FluxRecorder::detect(const PhotonPacket* pp, int l, double tau)
{
    int ell = _lambdagrid->ell(pp->lambda());
    int numScatt = pp->numScatt();
    double L = pp->luminosity();
    double Lext = L * exp(-tau);

    // record in SED arrays
    if (_includeFluxDensity)
    {
        if (_recordTotalOnly)
        {
            LockFree::add(_sed[Total][ell], L);
        }
        else
        {
            if (pp->hasPrimaryOrigin())
            {
                if (numScatt==0)
                {
                    LockFree::add(_sed[Transparent][ell], L);
                    LockFree::add(_sed[PrimaryDirect][ell], Lext);
                }
                else
                {
                    LockFree::add(_sed[PrimaryScattered][ell], Lext);
                    if (numScatt<=_numScatteringLevels)
                        LockFree::add(_sed[PrimaryScatteredLevel+numScatt-1][ell], Lext);
                }
            }
            else
            {
                if (numScatt==0) LockFree::add(_sed[SecondaryDirect][ell], Lext);
                else LockFree::add(_sed[SecondaryScattered][ell], Lext);
            }
        }
        if (_recordPolarization)
        {
            LockFree::add(_sed[TotalQ][ell], Lext*pp->stokesQ());
            LockFree::add(_sed[TotalU][ell], Lext*pp->stokesU());
            LockFree::add(_sed[TotalV][ell], Lext*pp->stokesV());
        }
    }

    // record in IFU arrays
    if (_includeSurfaceBrightness && l>=0)
    {
        size_t lell = l + ell*_numPixelsInFrame;

        if (_recordTotalOnly)
        {
            LockFree::add(_ifu[Total][lell], L);
        }
        else
        {
            if (pp->hasPrimaryOrigin())
            {
                if (numScatt==0)
                {
                    LockFree::add(_ifu[Transparent][lell], L);
                    LockFree::add(_ifu[PrimaryDirect][lell], Lext);
                }
                else
                {
                    LockFree::add(_ifu[PrimaryScattered][lell], Lext);
                    if (numScatt<=_numScatteringLevels)
                        LockFree::add(_ifu[PrimaryScatteredLevel+numScatt-1][lell], Lext);
                }
            }
            else
            {
                if (numScatt==0) LockFree::add(_ifu[SecondaryDirect][lell], Lext);
                else LockFree::add(_ifu[SecondaryScattered][lell], Lext);
            }
        }
        if (_recordPolarization)
        {
            LockFree::add(_ifu[TotalQ][lell], Lext*pp->stokesQ());
            LockFree::add(_ifu[TotalU][lell], Lext*pp->stokesU());
            LockFree::add(_ifu[TotalV][lell], Lext*pp->stokesV());
        }
    }

    // record statistics for both SEDs and IFUs
    if (_recordStatistics)
    {
        ContributionList& contributionList = _contributionLists.local();
        if (!contributionList.hasHistoryIndex(pp->historyIndex()))
        {
            recordContributions(contributionList);
            contributionList.reset(pp->historyIndex());
        }
        contributionList.addContribution(ell, l, Lext);
    }
}

////////////////////////////////////////////////////////////////////

void FluxRecorder::flush()
{
    // record the dangling contributions from all threads
    for (auto contributionList : _contributionLists.all())
    {
        recordContributions(*contributionList);
        contributionList->reset();
    }
}

////////////////////////////////////////////////////////////////////

void FluxRecorder::calibrateAndWrite()
{
    // collect recorded data from all processes
    for (auto& array : _sed) ProcessManager::sumToRoot(array);
    for (auto& array : _ifu) ProcessManager::sumToRoot(array);
    for (auto& array : _wsed) ProcessManager::sumToRoot(array);
    for (auto& array : _wifu) ProcessManager::sumToRoot(array);

    // calibrate and write only in the root process
    if (!ProcessManager::isRoot()) return;

    // calculate front factors for converting from recorded quantities to output quantities:
    //  - cFluxDensity converts from W/Hz to W/Hz/m2 (i.e. incorporating distance)
    //  - cSurfaceBrightness converts from W/Hz to W/Hz/m2/sr (i.e. incorporating distance and solid angle per pixel)
    double fourpid2 = 4.*M_PI * _distance*_distance;
    double omega = 4. * atan(0.5*_pixelSizeX/_distance) * atan(0.5*_pixelSizeY/_distance);
    double cFluxDensity = 1. / fourpid2;
    double cSurfaceBrightness = 1. / fourpid2 / omega;

    // convert from recorded quantities to output quantities and from internal units to user-selected output units
    // (for performance reasons, determine the units scaling factor only once for each wavelength)
    Units* units = _lambdagrid->find<Units>();
    int numWavelengths = _lambdagrid->numWavelengths();
    for (int ell=0; ell!=numWavelengths; ++ell)
    {
        // SEDs
        if (_includeFluxDensity)
        {
            double factor = cFluxDensity * units->ofluxdensityFrequency(_lambdagrid->lambda(ell), 1.);
            for (auto& array : _sed) if (array.size()) array[ell] *= factor;
        }
        // IFUs
        if (_includeSurfaceBrightness)
        {
            double factor = cSurfaceBrightness * units->osurfacebrightnessFrequency(_lambdagrid->lambda(ell), 1.);
            size_t begin = ell * _numPixelsInFrame;
            size_t end = begin + _numPixelsInFrame;
            for (auto& array : _ifu) if (array.size()) for (size_t lell=begin; lell!=end; ++lell) array[lell] *= factor;
        }
    }

    // write SEDs to a single text file (with multiple columns)
    if (_includeFluxDensity)
    {
        // Build a list of column names and corresponding pointers to sed arrays (which may be empty)
        vector<string> sedNames;
        vector<Array*> sedArrays;

        // add the total flux; if we didn't record it directly, calculate it now
        sedNames.push_back("total flux");
        Array sedTotal;
        if (_recordTotalOnly) sedArrays.push_back(&_sed[Total]);
        else
        {
            sedTotal = _sed[PrimaryDirect] + _sed[PrimaryScattered];
            if (_hasMediumEmission) sedTotal += _sed[SecondaryDirect] + _sed[SecondaryScattered];
            sedArrays.push_back(&sedTotal);
        }

        // add the flux components, if requested
        // we always add all of them, even if some of them are zero
        if (_recordComponents)
        {
            // add transparent flux
            // if we did not actually record components (because there are no media), use the total flux instead
            sedNames.push_back("transparent flux");
            sedArrays.push_back(_recordTotalOnly ? &_sed[Total] : &_sed[Transparent]);

            // add the actual components of the total flux
            sedNames.insert(sedNames.end(), {"direct primary flux", "scattered primary flux",
                                             "direct secondary flux", "scattered secondary flux"});
            sedArrays.insert(sedArrays.end(), {&_sed[PrimaryDirect], &_sed[PrimaryScattered],
                                               &_sed[SecondaryDirect], &_sed[SecondaryScattered]});
        }

        // add the polarization components, if requested
        if (_recordPolarization)
        {
            sedNames.insert(sedNames.end(), {"total Stokes Q", "total Stokes U", "total Stokes V"});
            sedArrays.insert(sedArrays.end(), {&_sed[TotalQ], &_sed[TotalU], &_sed[TotalV]});
        }

        // add the scattering levels, if requested
        if (!_recordTotalOnly) for (int i=0; i!=_numScatteringLevels; ++i)
        {
            sedNames.push_back(std::to_string(i+1) + "-times scattered primary flux");
            sedArrays.push_back(&_sed[PrimaryScatteredLevel+i]);
        }

        // open the file and add the column headers
        TextOutFile sedFile(_lambdagrid, _instrumentName + "_sed", "SED");
        sedFile.addColumn("lambda (" + units->uwavelength() + ")", 'e', 8);
        for (const string& name : sedNames)
        {
            sedFile.addColumn(name + "; " + units->sfluxdensity() + " " + "(" + units->ufluxdensity() + ")", 'e', 8);
        }

        // write the column data
        for (int ell=0; ell!=numWavelengths; ++ell)
        {
            vector<double> values({units->owavelength(_lambdagrid->lambda(ell))});
            for (const Array* array : sedArrays) values.push_back(array->size() ? (*array)[ell] : 0.);
            sedFile.writeRow(values);
        }
        sedFile.close();

        // TO DO: output statistics
    }

    // write IFUs to FITS files (one file per IFU)
    if (_includeSurfaceBrightness)
    {
        // Build a list of file names and corresponding pointers to ifu arrays (which may be empty)
        vector<string> ifuNames;
        vector<Array*> ifuArrays;

        // add the total flux; if we didn't record it directly, calculate it now
        ifuNames.push_back("total");
        Array ifuTotal;
        if (_recordTotalOnly) ifuArrays.push_back(&_ifu[Total]);
        else
        {
            ifuTotal = _ifu[PrimaryDirect] + _ifu[PrimaryScattered];
            if (_hasMediumEmission) ifuTotal += _ifu[SecondaryDirect] + _ifu[SecondaryScattered];
            ifuArrays.push_back(&ifuTotal);
        }

        // add the flux components, if requested
        if (_recordComponents)
        {
            // add the transparent flux only if it may differ from the total flux
            if (!_recordTotalOnly)
            {
                ifuNames.push_back("transparent");
                ifuArrays.push_back(&_ifu[Transparent]);
            }
            // add the actual components of the total flux (empty arrays will be ignored later on)
            ifuNames.insert(ifuNames.end(), {"primarydirect", "primaryscattered",
                                             "secondarydirect", "secondaryscattered"});
            ifuArrays.insert(ifuArrays.end(), {&_ifu[PrimaryDirect], &_ifu[PrimaryScattered],
                                               &_ifu[SecondaryDirect], &_ifu[SecondaryScattered]});
        }

        // add the polarization components, if requested
        if (_recordPolarization)
        {
            ifuNames.insert(ifuNames.end(), {"stokesQ", "stokesU", "stokesV"});
            ifuArrays.insert(ifuArrays.end(), {&_ifu[TotalQ], &_ifu[TotalU], &_ifu[TotalV]});
        }

        // add the scattering levels, if requested
        if (!_recordTotalOnly) for (int i=0; i!=_numScatteringLevels; ++i)
        {
            ifuNames.push_back("primaryscatteredlevel" + std::to_string(i+1));
            ifuArrays.push_back(&_ifu[PrimaryScatteredLevel+i]);
        }

        // output the files (ignoring empty arrays)
        int numFiles = ifuNames.size();
        for (int q=0; q!=numFiles; ++q) if (ifuArrays[q]->size())
        {
            string filename = _instrumentName + "_" + ifuNames[q];
            string description = ifuNames[q] + " flux";
            FITSInOut::write(_lambdagrid, description, filename, *(ifuArrays[q]),
                             _numPixelsX, _numPixelsY, numWavelengths,
                             units->olength(_pixelSizeX), units->olength(_pixelSizeY),
                             units->olength(_centerX), units->olength(_centerY),
                             units->usurfacebrightness(), units->ulength());
        }

        // TO DO: output statistics
    }
}

////////////////////////////////////////////////////////////////////

void FluxRecorder::recordContributions(ContributionList& contributionList)
{
    // sort the contributions on wavelength and pixel index so that contributions to the same bin are consecutive
    contributionList.sort();
    auto contributions = contributionList.contributions();
    auto numContributions = contributions.size();

    // for SEDs, group contributions on ell index (wavelength bin)
    if (_includeFluxDensity)
    {
        double w = 0;
        for (size_t i=0; i!=numContributions; ++i)
        {
            w += contributions[i].w();
            if (i+1 == numContributions || contributions[i].ell() != contributions[i+1].ell())
            {
                int ell = contributions[i].ell();
                double wn = 1.;
                for (int k=0; k!=numStatMoments; ++k)
                {
                    wn *= w;
                    LockFree::add(_wsed[k][ell], wn);
                }
                w = 0;
            }
        }
    }

    // for IFUs, group contributions on lell index (wavelength and pixel bins)
    if (_includeSurfaceBrightness)
    {
        double w = 0;
        for (size_t i=0; i!=numContributions; ++i)
        {
            w += contributions[i].w();
            if (i+1 == numContributions || contributions[i].ell() != contributions[i+1].ell()
                                        || contributions[i].l() != contributions[i+1].l())
            {
                size_t lell = contributions[i].l() + contributions[i].ell()*_numPixelsInFrame;
                double wn = 1.;
                for (int k=0; k!=numStatMoments; ++k)
                {
                    wn *= w;
                    LockFree::add(_wifu[k][lell], wn);
                }
                w = 0;
            }
        }
    }
}

////////////////////////////////////////////////////////////////////
