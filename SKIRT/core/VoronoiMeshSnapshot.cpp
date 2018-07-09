/*//////////////////////////////////////////////////////////////////
////     The SKIRT project -- advanced radiative transfer       ////
////       © Astronomical Observatory, Ghent University         ////
///////////////////////////////////////////////////////////////// */

#include "VoronoiMeshSnapshot.hpp"
#include "FatalError.hpp"
#include "Log.hpp"
#include "NR.hpp"
#include "Random.hpp"
#include "SiteListInterface.hpp"
#include "SpatialGridPath.hpp"
#include "StringUtils.hpp"
#include "TextInFile.hpp"
#include "Units.hpp"
#include "container.hh"

////////////////////////////////////////////////////////////////////

namespace VoronoiMesh_Private
{
    // class to hold the information about a Voronoi cell that is relevant for calculating paths and densities
    class VoronoiCell : public Box  // enclosing box
    {
    private:
        Vec _r;                     // site position
        Vec _c;                     // centroid position
        double _volume;             // volume
        vector<int> _neighbors;     // list of neighbor indices in cells vector

    public:
        // constructor stores the specified site position; the other data members are set to zero
        VoronoiCell(Vec r) : _r(r), _volume(0) { }

        // initializes the receiver with information taken from the specified fully computed Voronoi cell
        void init(voro::voronoicell_neighbor& cell)
        {
            // copy basic geometric info
            double cx, cy, cz;
            cell.centroid(cx,cy,cz);
            _c = Vec(cx,cy,cz) + _r;
            _volume = cell.volume();

            // get the minimal and maximal coordinates of the box enclosing the cell
            vector<double> coords;
            cell.vertices(_r.x(),_r.y(),_r.z(), coords);
            double xmin = DBL_MAX;  double ymin = DBL_MAX;  double zmin = DBL_MAX;
            double xmax = -DBL_MAX; double ymax = -DBL_MAX; double zmax = -DBL_MAX;
            int n = coords.size();
            for (int i=0; i<n; i+=3)
            {
                xmin = min(xmin,coords[i]); ymin = min(ymin,coords[i+1]); zmin = min(zmin,coords[i+2]);
                xmax = max(xmax,coords[i]); ymax = max(ymax,coords[i+1]); zmax = max(zmax,coords[i+2]);
            }

            // set our inherited Box to this bounding box
            setExtent(xmin, ymin, zmin, xmax, ymax, zmax);

            // copy a list of neighboring cell/site ids
            cell.neighbors(_neighbors);
        }

        // returns the cell's site position
        Vec position() const { return _r; }

        // returns the squared distance from the cell's site to the specified point
        double squaredDistanceTo(Vec r) const { return (r-_r).norm2(); }

        // returns the central position in the cell
        Vec centroid() const { return _c; }

        // returns the volume of the cell; overriding volume() function of Box bas class
        double volume() const { return _volume; }

        // returns a list of neighboring cell/site ids
        const vector<int>& neighbors() { return _neighbors; }
    };

    // function to compare two points according to the specified axis (0,1,2)
    bool lessthan(Vec p1, Vec p2, int axis)
    {
        switch (axis)
        {
        case 0:  // split on x
            if (p1.x()<p2.x()) return true;
            if (p1.x()>p2.x()) return false;
            if (p1.y()<p2.y()) return true;
            if (p1.y()>p2.y()) return false;
            if (p1.z()<p2.z()) return true;
            return false;
        case 1:  // split on y
            if (p1.y()<p2.y()) return true;
            if (p1.y()>p2.y()) return false;
            if (p1.z()<p2.z()) return true;
            if (p1.z()>p2.z()) return false;
            if (p1.x()<p2.x()) return true;
            return false;
        case 2:  // split on z
            if (p1.z()<p2.z()) return true;
            if (p1.z()>p2.z()) return false;
            if (p1.x()<p2.x()) return true;
            if (p1.x()>p2.x()) return false;
            if (p1.y()<p2.y()) return true;
            return false;
        default: // this should never happen
            return false;
        }
    }
}

////////////////////////////////////////////////////////////////////

namespace VoronoiMesh_Private
{
    // class to hold a node in the binary search tree (see en.wikipedia.org/wiki/Kd-tree)
    class Node
    {
    private:
        int _m;         // index in _cells to the site defining the split at this node
        int _axis;      // split axis for this node (0,1,2)
        Node* _up;      // ptr to the parent node
        Node* _left;    // ptr to the left child node
        Node* _right;   // ptr to the right child node

        // returns the square of its argument
        static double sqr(double x) { return x*x; }

    public:
        // constructor stores the specified site index and child pointers (which may be null)
        Node(int m, int depth, Node* left, Node* right) : _m(m), _axis(depth%3), _up(0), _left(left), _right(right)
        {
            if (_left) _left->setParent(this);
            if (_right) _right->setParent(this);
        }

        // destructor destroys the children
        ~Node() { delete _left; delete _right; }

        // sets parent pointer (called from parent's constructor)
        void setParent(Node* up) { _up = up; }

        // returns the corresponding data member
        int m() const { return _m; }
        Node* up() const { return _up; }
        Node* left() const { return _left; }
        Node* right() const { return _right; }

        // returns the apropriate child for the specified query point
        Node* child(Vec bfr, const vector<VoronoiCell*>& cells) const
            { return lessthan(bfr, cells[_m]->position(), _axis) ? _left : _right; }

        // returns the other child than the one that would be apropriate for the specified query point
        Node* otherChild(Vec bfr, const vector<VoronoiCell*>& cells) const
            { return lessthan(bfr, cells[_m]->position(), _axis) ? _right : _left; }

        // returns the squared distance from the query point to the split plane
        double squaredDistanceToSplitPlane(Vec bfr, const vector<VoronoiCell*>& cells) const
        {
            switch (_axis)
            {
            case 0:  // split on x
                return sqr(cells[_m]->position().x() - bfr.x());
            case 1:  // split on y
                return sqr(cells[_m]->position().y() - bfr.y());
            case 2:  // split on z
                return sqr(cells[_m]->position().z() - bfr.z());
            default: // this should never happen
                return 0;
            }
        }

        // returns the node in this subtree that represents the site nearest to the query point
        Node* nearest(Vec bfr, const vector<VoronoiCell*>& cells)
        {
            // recursively descend the tree until a leaf node is reached, going left or right depending on
            // whether the specified point is less than or greater than the current node in the split dimension
            Node* current = this;
            while (Node* child = current->child(bfr, cells)) current = child;

            // unwind the recursion, looking for the nearest node while climbing up
            Node* best = current;
            double bestSD = cells[best->m()]->squaredDistanceTo(bfr);
            while (true)
            {
                // if the current node is closer than the current best, then it becomes the current best
                double currentSD = cells[current->m()]->squaredDistanceTo(bfr);
                if (currentSD < bestSD)
                {
                    best = current;
                    bestSD = currentSD;
                }

                // if there could be points on the other side of the splitting plane for the current node
                // that are closer to the search point than the current best, then ...
                double splitSD = current->squaredDistanceToSplitPlane(bfr, cells);
                if (splitSD < bestSD)
                {
                    // move down the other branch of the tree from the current node looking for closer points,
                    // following the same recursive process as the entire search
                    Node* other = current->otherChild(bfr, cells);
                    if (other)
                    {
                        Node* otherBest = other->nearest(bfr, cells);
                        double otherBestSD = cells[otherBest->m()]->squaredDistanceTo(bfr);
                        if (otherBestSD < bestSD)
                        {
                            best = otherBest;
                            bestSD = otherBestSD;
                        }
                    }
                }

                // move up to the parent until we meet the top node
                if (current==this) break;
                current = current->up();
            }
            return best;
        }
    };
}

using namespace VoronoiMesh_Private;

////////////////////////////////////////////////////////////////////

namespace
{
    // function to recursively build the binary search tree (see en.wikipedia.org/wiki/Kd-tree)
    Node* buildTree(const vector<VoronoiCell*>& cells,
                    vector<int>::iterator first, vector<int>::iterator last, int depth)
    {
        auto length = last-first;
        if (length>0)
        {
            auto median = length >> 1;
            std::nth_element(first, first+median, last, [&cells, depth] (int m1, int m2)
                            { return m1!=m2 && lessthan(cells[m1]->position(), cells[m2]->position(), depth%3); });
            return new Node(*(first+median), depth,
                            buildTree(cells, first, first+median, depth+1),
                            buildTree(cells, first+median+1, last, depth+1));
        }
        return nullptr;
    }
}

////////////////////////////////////////////////////////////////////

VoronoiMeshSnapshot::VoronoiMeshSnapshot()
{
}

////////////////////////////////////////////////////////////////////

VoronoiMeshSnapshot::~VoronoiMeshSnapshot()
{
    for (auto cell : _cells) delete cell;
    for (auto tree : _blocktrees) delete tree;
}

////////////////////////////////////////////////////////////////////

void VoronoiMeshSnapshot::open(const SimulationItem* item, string filename, string description)
{
    Snapshot::open(item, filename, description);
    importPosition();
}

////////////////////////////////////////////////////////////////////

void VoronoiMeshSnapshot::readAndClose()
{
    // read the site info into memory
    //  -> if the user configured a temperature cutoff, we need to skip the "hot" sites
    //  -> in any case, we need to skip sites outside our domain
    double maxT  = hasTemperatureCutoff() ? maxTemperature() : 0.;
    int numIgnored = 0;
    Array prop;
    while (infile()->readRow(prop))
    {
        Vec r(prop[positionIndex()+0], prop[positionIndex()+1], prop[positionIndex()+2]);
        if (!_extent.contains(r) || (maxT && prop[temperatureIndex()] > maxT)) numIgnored++;
        else _propv.push_back(prop);
    }

    // close the file
    Snapshot::readAndClose();

    // log the number of sites
    if (!numIgnored)
    {
        log()->info("  Number of sites: " + std::to_string(_propv.size()));
    }
    else
    {
        log()->info("  Number of sites ignored: " + std::to_string(numIgnored));
        log()->info("  Number of sites retained: " + std::to_string(_propv.size()));
    }

    // calculate the Voronoi cells
    vector<Vec> sites;
    sites.reserve(_propv.size());
    for (const Array& prop : _propv)
        sites.emplace_back(prop[positionIndex()+0], prop[positionIndex()+1], prop[positionIndex()+2]);
    buildMesh(sites, false);

    // if a mass density policy has been set, calculate masses and densities for all cells
    if (hasMassDensityPolicy())
    {
        // allocate vectors for mass and density
        size_t n = _propv.size();
        Array Mv(n);
        _rhov.resize(n);

        // initialize statistics
        double totalOriginalMass = 0;
        double totalMetallicMass = 0;
        double totalEffectiveMass = 0;

        // loop over all sites/cells
        for (size_t m=0; m!=n; ++m)
        {
            const Array& prop = _propv[m];
            double originalMass = massIndex() ? prop[massIndex()] : prop[densityIndex()] * _cells[m]->volume();
            // guard against negative masses
            double metallicMass = max(0., originalMass * (metallicityIndex()>=0 ? prop[metallicityIndex()] : 1.));
            double effectiveMass = metallicMass * multiplier();

            Mv[m] = effectiveMass;
            _rhov[m] = effectiveMass / _cells[m]->volume();

            totalOriginalMass += originalMass;
            totalMetallicMass += metallicMass;
            totalEffectiveMass += effectiveMass;
        }

        // log mass statistics
        log()->info("  Total original mass : " +
                    StringUtils::toString(units()->omass(totalOriginalMass),'e',4) + " " + units()->umass());
        log()->info("  Total metallic mass : "
                    + StringUtils::toString(units()->omass(totalMetallicMass),'e',4) + " " + units()->umass());
        log()->info("  Total effective mass: "
                    + StringUtils::toString(units()->omass(totalEffectiveMass),'e',4) + " " + units()->umass());

        // if one of the total masses is negative, suppress the complete mass distribution
        if (totalOriginalMass < 0 || totalMetallicMass < 0 || totalEffectiveMass < 0)
        {
            log()->warning("  Total imported mass is negative; suppressing the complete mass distribution");
            totalEffectiveMass = 0;
            _propv.clear();
            _rhov.resize(0);
            _cells.clear();
        }

        // remember the effective mass
        _mass = totalEffectiveMass;

        // if there is at least one site...
        if (!_propv.empty())
        {
            // construct a vector with the normalized cumulative site densities
            NR::cdf(_cumrhov, Mv);

            // build the search data structure
            buildSearch();
        }
    }
}

////////////////////////////////////////////////////////////////////

void VoronoiMeshSnapshot::setExtent(const Box& extent)
{
    _extent = extent;
    _eps = 1e-12 * extent.widths().norm();
}

////////////////////////////////////////////////////////////////////

VoronoiMeshSnapshot::VoronoiMeshSnapshot(const SimulationItem* item, const Box& extent, std::string filename)
{
    setContext(item);
    setExtent(extent);

    // read the input file
    TextInFile in(item, filename, "Voronoi sites");
    in.addColumn("position x", "length", "pc");
    in.addColumn("position y", "length", "pc");
    in.addColumn("position z", "length", "pc");
    auto propv = in.readAllRows();
    in.close();

    // prepare the data
    vector<Vec> sites;
    sites.reserve(_propv.size());
    for (const Array& prop : propv) sites.emplace_back(prop[0], prop[1], prop[2]);

    // calculate the Voronoi cells
    buildMesh(sites, true);
    buildSearch();
}

////////////////////////////////////////////////////////////////////

VoronoiMeshSnapshot::VoronoiMeshSnapshot(const SimulationItem* item, const Box& extent, SiteListInterface* sli)
{
    setContext(item);
    setExtent(extent);

    // prepare the data
    int n = sli->numSites();
    vector<Vec> sites(n);
    for (int m=0; m!=n; ++m) sites[m] = sli->sitePosition(m);

    // calculate the Voronoi cells
    buildMesh(sites, true);
    buildSearch();
}

////////////////////////////////////////////////////////////////////

VoronoiMeshSnapshot::VoronoiMeshSnapshot(const SimulationItem* item, const Box& extent, const vector<Vec>& sites)
{
    setContext(item);
    setExtent(extent);

    // calculate the Voronoi cells
    buildMesh(sites, true);
    buildSearch();
}

////////////////////////////////////////////////////////////////////

void VoronoiMeshSnapshot::buildMesh(const vector<Vec>& sites, bool ignoreNearbyAndOutliers)
{
    // create a list of site indices
    int numSites =  sites.size();
    vector<int> indices(numSites);
    for (int m=0; m!=numSites; ++m) indices[m] = m;

    // if requested to remove nearby sites and sites outside of the domain...
    int numIgnored = 0;
    if (ignoreNearbyAndOutliers)
    {
        // sort the list of site indices on the site's x coordinate
        std::sort(indices.begin(), indices.end(),
                  [&sites] (int m1, int m2) { return sites[m1].x() < sites[m2].x(); });

        // find sites that violate the conditions and tag them for removal
        for (int i=0; i!=numSites; ++i)
        {
            // outside of the domain
            if (!_extent.contains(sites[indices[i]]))
            {
                indices[i] = -1;
                numIgnored++;
            }
            else
            {
                // too nearby
                for (int j = i+1; j < numSites && sites[indices[j]].x()-sites[indices[i]].x() < _eps; ++j)
                {
                    if ((sites[indices[j]]-sites[indices[i]]).norm2() < _eps*_eps)
                    {
                        indices[i] = -1;
                        numIgnored++;
                        break;
                    }
                }
            }
        }

        // log the number of sites
        if (!numIgnored)
        {
            log()->info("  Number of sites: " + std::to_string(numSites));
        }
        else
        {
            log()->info("  Number of sites ignored: " + std::to_string(numIgnored));
            log()->info("  Number of sites retained: " + std::to_string(numSites-numIgnored));
        }
    }

    // abort if there are no cells to calculate
    int numCells = numSites - numIgnored;
    if (numCells <= 0) return;

    // initialize the vector that will hold pointers to the cell objects that will stay around,
    // using the serial number of the cell as index in the vector
    _cells.resize(numCells);

    // add the specified sites to our cell vector AND to a temporary Voronoi container,
    // using the serial number of the cell as site ID
    int nb = max(3, min(1000, static_cast<int>(3.*pow(numCells,1./3.)) ));
    voro::container con(_extent.xmin(), _extent.xmax(), _extent.ymin(), _extent.ymax(), _extent.zmin(), _extent.zmax(),
                        nb, nb, nb, false,false,false, 8);
    int m = 0;
    for (int i=0; i!=numSites; ++i)
    {
        if (indices[i]>=0)                      // skip sites that were tagged for removal
        {
            Vec r = sites[indices[i]];
            _cells[m] = new VoronoiCell(r);     // these objects will be deleted by the destructor
            con.put(m, r.x(),r.y(),r.z());
            m++;
        }
    }

    // for each site:
    //   - compute the corresponding cell in the Voronoi tesselation
    //   - extract and copy the relevant information to one of our own cell objects
    //   - store the cell object in the vector indexed on cell number
    int ii = 0;
    voro::c_loop_all loop(con);
    if (loop.start()) do
    {
        if (ii%10000==0) log()->info("Computing Voronoi cell " + std::to_string(ii+1) + "...");

        // Compute the cell
        voro::voronoicell_neighbor fullcell;
        bool ok = con.compute_cell(fullcell, loop);
        if (!ok) throw FATALERROR("Can't compute Voronoi cell");

        // Copy all relevant information to the cell object that will stay around
        VoronoiCell* cell = _cells[loop.pid()];
        cell->init(fullcell);

        ii++;
    }
    while (loop.inc());

    // compile neighbor statistics
    int minNeighbors = INT_MAX;
    int maxNeighbors = 0;
    int64_t totNeighbors = 0;
    for (int m=0; m<numCells; m++)
    {
        int ns = _cells[m]->neighbors().size();
        totNeighbors += ns;
        minNeighbors = min(minNeighbors, ns);
        maxNeighbors = max(maxNeighbors, ns);
    }
    double avgNeighbors = double(totNeighbors)/numCells;

    // log neighbor statistics
    log()->info("Computed Voronoi tessellation with " + std::to_string(numCells) + " cells");
    log()->info("  Average number of neighbors per cell: " + StringUtils::toString(avgNeighbors,'f',1));
    log()->info("  Minimum number of neighbors per cell: " + std::to_string(minNeighbors));
    log()->info("  Maximum number of neighbors per cell: " + std::to_string(maxNeighbors));
}

////////////////////////////////////////////////////////////////////

void VoronoiMeshSnapshot::buildSearch()
{
    // abort if there are no cells
    int numCells = _cells.size();
    if (!numCells) return;

    log()->info("Building data structures to accelerate searching the Voronoi tesselation...");

    // -------------  block lists  -------------

    // initialize a vector of nb x nb x nb lists, each containing the cells overlapping a certain block in the domain
    _nb = max(3, min(1000, static_cast<int>(3.*pow(numCells,1./3.)) ));
    _nb2 = _nb*_nb;
    _nb3 = _nb*_nb*_nb;
    _blocklists.resize(_nb3);

    // add the cell object to the lists for all blocks it may overlap
    int i1,j1,k1, i2,j2,k2;
    for (int m=0; m!=numCells; ++m)
    {
        _extent.cellIndices(i1,j1,k1, _cells[m]->rmin()-Vec(_eps,_eps,_eps), _nb,_nb,_nb);
        _extent.cellIndices(i2,j2,k2, _cells[m]->rmax()+Vec(_eps,_eps,_eps), _nb,_nb,_nb);
        for (int i=i1; i<=i2; i++)
            for (int j=j1; j<=j2; j++)
                for (int k=k1; k<=k2; k++)
                    _blocklists[i*_nb2+j*_nb+k].push_back(m);
    }

    // compile block list statistics
    int minRefsPerBlock = INT_MAX;
    int maxRefsPerBlock = 0;
    int64_t totalBlockRefs = 0;
    for (int b = 0; b<_nb3; b++)
    {
        int refs = _blocklists[b].size();
        totalBlockRefs += refs;
        minRefsPerBlock = min(minRefsPerBlock, refs);
        maxRefsPerBlock = max(maxRefsPerBlock, refs);
    }
    double avgRefsPerBlock = double(totalBlockRefs)/_nb3;

    // log block list statistics
    log()->info("  Number of blocks in search grid: " + std::to_string(_nb3) + " (" + std::to_string(_nb) + "^3)");
    log()->info("  Average number of cells per block: " + StringUtils::toString(avgRefsPerBlock,'f',1));
    log()->info("  Minimum number of cells per block: " + std::to_string(minRefsPerBlock));
    log()->info("  Maximum number of cells per block: " + std::to_string(maxRefsPerBlock));

    // -------------  search trees  -------------

    // for each block that contains more than a predefined number of cells,
    // construct a search tree on the site locations of the cells
    _blocktrees.resize(_nb3);
    for (int b = 0; b<_nb3; b++)
    {
        vector<int>& ids = _blocklists[b];
        if (ids.size() > 5) _blocktrees[b] = buildTree(_cells, ids.begin(), ids.end(), 0);
    }

    // compile search tree statistics
    int minRefsPerTree = INT_MAX;
    int maxRefsPerTree = 0;
    int64_t totalTreeRefs = 0;
    int numTrees = 0;
    for (int b = 0; b<_nb3; b++)
    {
        if (_blocktrees[b])
        {
            numTrees++;
            int refs = _blocklists[b].size();
            totalTreeRefs += refs;
            minRefsPerTree = min(minRefsPerTree, refs);
            maxRefsPerTree = max(maxRefsPerTree, refs);
        }
    }
    double avgRefsPerTree = double(totalTreeRefs)/numTrees;

    // log search tree statistics
    log()->info("  Number of search trees: " + std::to_string(numTrees) +
              " (" + StringUtils::toString(100.*numTrees/_nb3,'f',1) + "% of blocks)");
    if (numTrees)
    {
        log()->info("  Average number of cells per tree: " + StringUtils::toString(avgRefsPerTree,'f',1));
        log()->info("  Minimum number of cells per tree: " + std::to_string(minRefsPerTree));
        log()->info("  Maximum number of cells per tree: " + std::to_string(maxRefsPerTree));
    }
}

////////////////////////////////////////////////////////////////////

Box VoronoiMeshSnapshot::extent() const
{
    return _extent;
}

////////////////////////////////////////////////////////////////////

int VoronoiMeshSnapshot::numEntities() const
{
    return _cells.size();
}

////////////////////////////////////////////////////////////////////

Position VoronoiMeshSnapshot::position(int m) const
{
    return Position(_cells[m]->position());
}

////////////////////////////////////////////////////////////////////

Position VoronoiMeshSnapshot::centroidPosition(int m) const
{
    return Position(_cells[m]->centroid());
}

////////////////////////////////////////////////////////////////////

double VoronoiMeshSnapshot::volume(int m) const
{
    return _cells[m]->volume();
}

////////////////////////////////////////////////////////////////////

Box VoronoiMeshSnapshot::extent(int m) const
{
    return _cells[m]->extent();
}

////////////////////////////////////////////////////////////////////

Vec VoronoiMeshSnapshot::velocity(int m) const
{
    return Vec(_propv[m][velocityIndex()+0], _propv[m][velocityIndex()+1], _propv[m][velocityIndex()+2]);
}

////////////////////////////////////////////////////////////////////

void VoronoiMeshSnapshot::parameters(int m, Array& params) const
{
    int n = numParameters();
    params.resize(n);
    for (int i=0; i!=n; ++i) params[i] = _propv[m][parametersIndex()+i];
}

////////////////////////////////////////////////////////////////////

namespace
{
    // returns true if the given point is closer to the site with index m than to the sites with indices ids
    bool isPointClosestTo(const vector<VoronoiCell*>& cells, Vec r, int m, const vector<int>& ids)
    {
        double target = cells[m]->squaredDistanceTo(r);
        for (int id : ids)
        {
            if (id>=0 && cells[id]->squaredDistanceTo(r) < target) return false;
        }
        return true;
    }
}

////////////////////////////////////////////////////////////////////

Position VoronoiMeshSnapshot::generatePosition(int m) const
{
    // get loop-invariant information about the cell
    const Box& box = _cells[m]->extent();
    const vector<int>& neighbors = _cells[m]->neighbors();

    // generate random points in the enclosing box until one happens to be inside the cell
    for (int i=0; i<10000; i++)
    {
        Position r = random()->position(box);
        if (isPointClosestTo(_cells, r, m, neighbors)) return r;
    }
    throw FATALERROR("Can't find random position in cell");
}

////////////////////////////////////////////////////////////////////

double VoronoiMeshSnapshot::density(int m) const
{
    return _rhov[m];
}

////////////////////////////////////////////////////////////////////

double VoronoiMeshSnapshot::mass() const
{
    return _mass;
}

////////////////////////////////////////////////////////////////////

int VoronoiMeshSnapshot::cellIndex(Position bfr) const
{
    // make sure the position is inside the domain
    if (!_extent.contains(bfr)) return -1;

    // determine the block in which the point falls
    int i,j,k;
    _extent.cellIndices(i,j,k, bfr, _nb,_nb,_nb);
    int b = i*_nb2+j*_nb+k;

    // look for the closest site in this block, using the search tree if there is one
    Node* tree = _blocktrees[b];
    if (tree) return tree->nearest(bfr,_cells)->m();

    // if there is no search tree, simply loop over the index list
    const vector<int>& ids = _blocklists[b];
    int m = -1;
    double mdist = DBL_MAX;
    int n = ids.size();
    for (int i=0; i<n; i++)
    {
        double idist = _cells[ids[i]]->squaredDistanceTo(bfr);
        if (idist < mdist)
        {
            m = ids[i];
            mdist = idist;
        }
    }
    return m;
}

////////////////////////////////////////////////////////////////////

double VoronoiMeshSnapshot::density(Position bfr) const
{
    int m = cellIndex(bfr);
    return m>=0 ? _rhov[m] : 0;
}

////////////////////////////////////////////////////////////////////

Position VoronoiMeshSnapshot::generatePosition() const
{
    // if there are no sites, return the origin
    if (_cells.empty()) return Position();

    // select a site according to its mass contribution
    int m = NR::locateClip(_cumrhov, random()->uniform());

    return generatePosition(m);
}

////////////////////////////////////////////////////////////////////

void VoronoiMeshSnapshot::path(SpatialGridPath* path) const
{
    // Initialize the path
    path->clear();
    Direction bfk = path->direction();

    // If the photon package starts outside the dust grid, move it into the first grid cell that it will pass
    Position r = path->moveInside(_extent, _eps);

    // Get the index of the cell containing the current position;
    // if the position is not inside the grid, return an empty path
    int mr = cellIndex(r);
    if (mr<0) return path->clear();

    // Start the loop over cells/path segments until we leave the grid
    while (mr>=0)
    {
        // get the site position for this cell
        Vec pr = _cells[mr]->position();

        // initialize the smallest nonnegative intersection distance and corresponding index
        double sq = DBL_MAX;          // very large, but not infinity (so that infinite si values are discarded)
        const int NO_INDEX = -99;     // meaningless cell index
        int mq = NO_INDEX;

        // loop over the list of neighbor indices
        const vector<int>& mv = _cells[mr]->neighbors();
        int n = mv.size();
        for (int i=0; i<n; i++)
        {
            int mi = mv[i];

            // declare the intersection distance for this neighbor (init to a value that will be rejected)
            double si = 0;

            // --- intersection with neighboring cell
            if (mi>=0)
            {
                // get the site position for this neighbor
                Vec pi = _cells[mi]->position();

                // calculate the (unnormalized) normal on the bisecting plane
                Vec n = pi - pr;

                // calculate the denominator of the intersection quotient
                double ndotk = Vec::dot(n,bfk);

                // if the denominator is negative the intersection distance is negative, so don't calculate it
                if (ndotk > 0)
                {
                    // calculate a point on the bisecting plane
                    Vec p = 0.5 * (pi + pr);

                    // calculate the intersection distance
                    si = Vec::dot(n,p-r) / ndotk;
                }
            }

            // --- intersection with domain wall
            else
            {
                switch (mi)
                {
                case -1: si = (extent().xmin()-r.x())/bfk.x(); break;
                case -2: si = (extent().xmax()-r.x())/bfk.x(); break;
                case -3: si = (extent().ymin()-r.y())/bfk.y(); break;
                case -4: si = (extent().ymax()-r.y())/bfk.y(); break;
                case -5: si = (extent().zmin()-r.z())/bfk.z(); break;
                case -6: si = (extent().zmax()-r.z())/bfk.z(); break;
                default: throw FATALERROR("Invalid neighbor ID");
                }
            }

            // remember the smallest nonnegative intersection point
            if (si > 0 && si < sq)
            {
                sq = si;
                mq = mi;
            }
        }

        // if no exit point was found, advance the current point by small distance and recalculate cell index
        if (mq == NO_INDEX)
        {
            r += bfk*_eps;
            mr = cellIndex(r);
        }
        // otherwise add a path segment and set the current point to the exit point
        else
        {
            path->addSegment(mr, sq);
            r += (sq+_eps)*bfk;
            mr = mq;
        }
    }
}

////////////////////////////////////////////////////////////////////
