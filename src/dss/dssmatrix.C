/*
 *
 *                 #####    #####   ######  ######  ###   ###
 *               ##   ##  ##   ##  ##      ##      ## ### ##
 *              ##   ##  ##   ##  ####    ####    ##  #  ##
 *             ##   ##  ##   ##  ##      ##      ##     ##
 *            ##   ##  ##   ##  ##      ##      ##     ##
 *            #####    #####   ##      ######  ##     ##
 *
 *
 *             OOFEM : Object Oriented Finite Element Code
 *
 *               Copyright (C) 1993 - 2013   Borek Patzak
 *
 *
 *
 *       Czech Technical University, Faculty of Civil Engineering,
 *   Department of Structural Mechanics, 166 29 Prague, Czech Republic
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "dssmatrix.h"
#include "error.h"
#include "engngm.h"
#include "domain.h"
#include "element.h"
#include "dofmanager.h"
#include "dof.h"
#include "sparsemtrxtype.h"
#include "classfactory.h"
#include "activebc.h"

#include <set>

namespace oofem {

REGISTER_SparseMtrx( DSSMatrixLDL, SMT_DSS_sym_LDL);
REGISTER_SparseMtrx( DSSMatrixLL, SMT_DSS_sym_LL);
REGISTER_SparseMtrx( DSSMatrixLU, SMT_DSS_unsym_LU);


DSSMatrix :: DSSMatrix(dssType _t, int n) : SparseMtrx(n, n),
    _dss(std::make_unique<DSSolver>()),
    isFactorized(false),
    _type(_t)
{
    eDSSolverType _st = eDSSFactorizationLDLT;
    if ( _t == sym_LDL ) {
        _st = eDSSFactorizationLDLT;
    } else if ( _t == sym_LL ) {
        _st = eDSSFactorizationLLT;
    } else if ( _t == unsym_LU ) {
        _st = eDSSFactorizationLU;
    } else {
        OOFEM_ERROR("unknown dssType");
    }

    _dss->Initialize(0, _st);
}


/*****************************/
/*  Copy constructor         */
/*****************************/

DSSMatrix :: DSSMatrix(const DSSMatrix &S) : SparseMtrx(S.nRows, S.nColumns)
{
    OOFEM_ERROR("not implemented");
}


void DSSMatrix :: times(const FloatArray &x, FloatArray &answer) const
{
    // Note: not really eficient. The sparse matrix is assembled directly into its block structure,
    // which is efficient for factorization, but unfortunately not efficient for implementing the multiplication,
    // as the blocks have to be identified (see implementation of ElementAt method) when traversing rows
    // Also note, that this method will yield correct results only before factorization, after that the blocks
    // contain factorized matrix.

    int dim = this->_sm->neq;

    answer.resize(dim);
    answer.zero();

    for ( int i = 1; i <= dim; i++) {
        for ( int j = 1; j <= dim; j++) {
            answer.at(i) += _dss->ElementAt(i - 1, j - 1) * x.at(j);  
        }
    }
}

void DSSMatrix :: times(double x)
{
    _dss->times(x);
}

int DSSMatrix :: buildInternalStructure(EngngModel *eModel, int di, const UnknownNumberingScheme &s)
{
    IntArray loc;
    Domain *domain = eModel->giveDomain(di);
    int neq = eModel->giveNumberOfDomainEquations(di, s);
    unsigned long indx;
    // allocation map
    std :: vector< IntArray > columns(neq);

    for ( auto &elem : domain->giveElements() ) {
        elem->giveLocationArray(loc, s);

        for ( int ii : loc ) {
            if ( ii > 0 ) {
                for ( int jj : loc ) {
                    if ( jj > 0 ) {
                        columns [ jj - 1 ].insertSortedOnce(ii - 1);
                    }
                }
            }
        }
    }

    for ( auto &gbc : domain->giveBcs() ) {
        ActiveBoundaryCondition *bc = dynamic_cast< ActiveBoundaryCondition * >( gbc.get() );
        if ( bc ) {
            std::vector<IntArray> r_locs;
            std::vector<IntArray> c_locs;
            bc->giveLocationArrays(r_locs, c_locs, UnknownCharType, s, s);
            for (std::size_t k = 0; k < r_locs.size(); k++) {
                IntArray &krloc = r_locs[k];
                IntArray &kcloc = c_locs[k];
                for ( int ii : krloc ) {
                    if ( ii > 0 ) {
                        for ( int jj : kcloc ) {
                            if ( jj > 0 ) {
                                columns [ jj - 1 ].insertSortedOnce(ii - 1);
                            }
                        }
                    }
                }
            }
        }
    }

    unsigned long nz_ = 0;
    for ( int i = 0; i < neq; i++ ) {
        nz_ += columns [ i ].giveSize();
    }

    rowind_.reset( new unsigned long [ nz_ ]);
    colptr_.reset( new unsigned long [ neq + 1 ]);
    if ( ( rowind_ == NULL ) || ( colptr_ == NULL ) ) {
        OOFEM_ERROR("free store exhausted, exiting");
    }

    indx = 0;

    for ( int j = 0; j < neq; j++ ) { // column loop
      colptr_ [ j ] = indx;
        for ( auto &val : columns [ j ] ) { // row loop
            rowind_ [ indx++ ] = val;
        }
    }

    colptr_ [ neq ] = indx;

    _sm.reset( new SparseMatrixF(neq, NULL, rowind_.get(), colptr_.get(), 0, 0, true) ); 
    if ( !_sm ) {
        OOFEM_FATAL("free store exhausted, exiting");
    }


    /*
     *  Assemble block to equation mapping information
     */

    bool _succ = true;
    int _ndofs, _neq, ndofmans = domain->giveNumberOfDofManagers();
    int ndofmansbc = 0, nInternalElementDofMans=0;

    // count number of internal dofmans on active bc
    for ( auto &bc : domain->giveBcs() ) {
        ndofmansbc += bc->giveNumberOfInternalDofManagers();
    }
    // count element internal dofmans
    for ( auto &elem : domain->giveElements() ) {
      nInternalElementDofMans += elem->giveNumberOfInternalDofManagers();
    }

    int bsize = 0;
    if ( ndofmans > 0 ) {
        bsize = domain->giveDofManager(1)->giveNumberOfDofs();
    }

    long *mcn = new long [ (ndofmans+ndofmansbc+nInternalElementDofMans) * bsize ];
    long _c = 0;

    if ( mcn == NULL ) {
        OOFEM_FATAL("free store exhausted, exiting");
    }

    for ( auto &dman : domain->giveDofManagers() ) {
        _ndofs = dman->giveNumberOfDofs();
        if ( _ndofs > bsize ) {
            _succ = false;
            break;
        }

        for ( Dof *dof: *dman ) {
            if ( dof->isPrimaryDof() ) {
                _neq = dof->giveEquationNumber(s);
                if ( _neq > 0 ) {
                    mcn [ _c++ ] = _neq - 1;
                } else {
                    mcn [ _c++ ] = -1; // no corresponding row in sparse mtrx structure
                }
            } else {
                    mcn [ _c++ ] = -1; // no corresponding row in sparse mtrx structure
            }
        }

        for ( int i = _ndofs + 1; i <= bsize; i++ ) {
            mcn [ _c++ ] = -1;                         // no corresponding row in sparse mtrx structure
        }
    }

    // loop over internal dofmans of active bc
    for ( auto &bc : domain->giveBcs() ) {
      int ndman = bc->giveNumberOfInternalDofManagers();
      for (int idman = 1; idman <= ndman; idman ++) {
            DofManager *dman = bc->giveInternalDofManager(idman);
            _ndofs = dman->giveNumberOfDofs();
            if ( _ndofs > bsize ) {
                _succ = false;
                break;
            }

            for ( Dof *dof: *dman ) {
                if ( dof->isPrimaryDof() ) {
                    _neq = dof->giveEquationNumber(s);
                    if ( _neq > 0 ) {
                    mcn [ _c++ ] = _neq - 1;
                    } else {
                    mcn [ _c++ ] = -1; // no corresponding row in sparse mtrx structure
                    }
                }
            }

            for ( int i = _ndofs + 1; i <= bsize; i++ ) {
                mcn [ _c++ ] = -1;                         // no corresponding row in sparse mtrx structure
            }
        }
    }

    // loop over internal element dofs
    for ( auto &elem : domain->giveElements() ) {
      int ndman = elem->giveNumberOfInternalDofManagers();
      for (int idman = 1; idman <= ndman; idman ++) {
            DofManager *dman = elem->giveInternalDofManager(idman);
            _ndofs = dman->giveNumberOfDofs();
            if ( _ndofs > bsize ) {
                _succ = false;
                break;
            }

            for ( Dof *dof: *dman ) {
                if ( dof->isPrimaryDof() ) {
                    _neq = dof->giveEquationNumber(s);
                    if ( _neq > 0 ) {
                        mcn [ _c++ ] = _neq - 1;
                    } else {
                        mcn [ _c++ ] = -1; // no corresponding row in sparse mtrx structure
                    }
                }
            }

            for ( int i = _ndofs + 1; i <= bsize; i++ ) {
                mcn [ _c++ ] = -1;                         // no corresponding row in sparse mtrx structure
            }
        }
    }

    if ( _succ ) {
        _dss->SetMatrixPattern(_sm.get(), bsize);
        _dss->LoadMCN(ndofmans+ndofmansbc+nInternalElementDofMans, bsize, mcn);
    } else {
        OOFEM_LOG_INFO("DSSMatrix: using assumed block structure");
        _dss->SetMatrixPattern(_sm.get(), bsize);
    }

    _dss->PreFactorize();
    // zero matrix, put unity on diagonal with supported dofs
    _dss->LoadZeros();
    delete[] mcn;

    OOFEM_LOG_DEBUG("DSSMatrix info: neq is %d, bsize is %d\n", neq, nz_);

    // increment version
    this->version++;

    return true;
}


int DSSMatrix :: assemble(const IntArray &loc, const FloatMatrix &mat)
{
    int dim = mat.giveNumberOfRows();

#  ifdef DEBUG
    if ( dim != loc.giveSize() ) {
        OOFEM_ERROR("dimension of 'k' and 'loc' mismatch");
    }
 #  endif

    if ( _type == unsym_LU ) {
        for ( int j = 1; j <= dim; j++ ) {
            int jj = loc.at(j);
            if ( jj ) {
                for ( int i = 1; i <= dim; i++ ) {
                    int ii = loc.at(i);
                    if ( ii ) {
                        _dss->ElementAt(ii - 1, jj - 1) += mat.at(i, j);
                    }
                }
            }
        }
    } else { // symmetric pattern
        for ( int j = 1; j <= dim; j++ ) {
            int jj = loc.at(j);
            if ( jj ) {
                for ( int i = 1; i <= dim; i++ ) {
                    int ii = loc.at(i);
                    if ( ii ) {
                        if ( jj > ii ) {
                            continue;
                        }

                        _dss->ElementAt(jj - 1, ii - 1) += mat.at(j, i);
                    }
                }
            }
        }
    }

    this->version++;

    return 1;
}

int DSSMatrix :: assemble(const IntArray &rloc, const IntArray &cloc, const FloatMatrix &mat)
{
    int dim1 = mat.giveNumberOfRows();
    int dim2 = mat.giveNumberOfColumns();
    if ( _type == unsym_LU ) {
        for ( int i = 1; i <= dim1; i++ ) {
            int ii = rloc.at(i);
            if ( ii ) {
                for ( int j = 1; j <= dim2; j++ ) {
                    int jj = cloc.at(j);
                    if ( jj ) {
                        _dss->ElementAt(ii - 1, jj - 1) += mat.at(i, j);
                    }
                }
            }
        }
    } else { // symmetric pattern
        for ( int i = 1; i <= dim1; i++ ) {
            int ii = rloc.at(i);
            if ( ii ) {
                for ( int j = 1; j <= dim2; j++ ) {
                    int jj = cloc.at(j);
                    if ( jj && (jj <= ii) ) {
                        _dss->ElementAt(ii - 1, jj - 1) += mat.at(i, j);
                    }
                }
            }
        }
    }

    this->version++;

    return 1;
}

void DSSMatrix :: zero()
{
    _dss->LoadZeros();

    this->version++;
    isFactorized = false;
}

SparseMtrx *DSSMatrix :: factorized()
{
    if ( isFactorized ) {
        return this;
    }

    _dss->ReFactorize();
    isFactorized = true;
    return this;
}

void DSSMatrix :: solve(FloatArray &b, FloatArray &x)
{
    x.resize( b.giveSize() );
    _dss->Solve( x.givePointer(), b.givePointer() );
}

/*********************/
/*   Array access    */
/*********************/

double &DSSMatrix :: at(int i, int j)
{
    this->version++;
    return _dss->ElementAt(i - 1, j - 1);
}


double DSSMatrix :: at(int i, int j) const
{
    return _dss->ElementAt(i - 1, j - 1);
}

double DSSMatrix :: operator() (int i, int j)  const
{
    return _dss->ElementAt(i, j);
}

double &DSSMatrix :: operator() (int i, int j)
{
    this->version++;
    return _dss->ElementAt(i, j);
}
} // end namespace oofem

