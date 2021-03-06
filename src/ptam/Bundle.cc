// Copyright 2008 Isis Innovation Limited
#include "Bundle.h"
#include "MEstimator.h"
#include <TooN/helpers.h>
#include <TooN/Cholesky.h>
#include <fstream>
#include <iomanip>
#include <gvars3/instances.h>

using namespace GVars3;
using namespace std;
using namespace ptam;
#ifdef WIN32
inline bool isnan(double d) {return !(d==d);}
#endif

#define cout if(*mgvnBundleCout) cout

// Some inlines which replace standard matrix multiplications 
// with LL-triangle-only versions.
inline void BundleTriangle_UpdateM6U_LL(Matrix<6> &m6U, Matrix<2,6> &m26A)
{
    for(int r=0; r<6; r++)
        for(int c=0; c<=r; c++)
            m6U(r,c)+= m26A.T()(r,0)*m26A(0,c) + m26A.T()(r,1)*m26A(1,c);
}
inline void BundleTriangle_UpdateM3V_LL(Matrix<3> &m3V, Matrix<2,3> &m23B)
{
    for(int r=0; r<3; r++)
        for(int c=0; c<=r; c++)
            m3V(r,c)+= m23B.T()(r,0)*m23B(0,c) + m23B.T()(r,1)*m23B(1,c);
}

// Constructor copies MapMaker's camera parameters
Bundle::Bundle()
    : mCamera(CameraModel::CreateCamera())
{
    for (int i = 0; i < AddCamNumber; i ++){
        std::auto_ptr<CameraModel> camera_temp (CameraModel::CreateCamera(i + 1));
        mCameraSec[i] = camera_temp;
    }
    mnCamsToUpdate = 0;
    mnStartRow = 0;
    GV3::Register(mgvnMaxIterations, "Bundle.MaxIterations", 20,  SILENT);
    GV3::Register(mgvdUpdateConvergenceLimit, "Bundle.UpdateSquaredConvergenceLimit", 1e-06, SILENT);
    GV3::Register(mgvnBundleCout, "Bundle.Cout", 0, SILENT);
};

Bundle::~Bundle()
{
    for (std::list<Meas*>::iterator it = mMeasList.begin(); it != mMeasList.end(); it++) {
        Meas* m = *it;
        delete m;
    }
}

// Add a camera to the system, return value is the bundle adjuster's ID for the camera
int Bundle::AddCamera(SE3<> se3CamFromWorld, bool bFixed, SE3<> se3C2FromC1)
{
    int n = mvCameras.size();
    Camera c;
    c.bFixed = bFixed;
    c.se3CfW = se3CamFromWorld;
    c.se3C2fC1 = se3C2FromC1;
    if(!bFixed)
    {
        c.nStartRow = mnStartRow;
        mnStartRow += 6;
        mnCamsToUpdate++;
    }
    else
        c.nStartRow = -999999999;
    mvCameras.push_back(c);

    return n;
}

// Add a map point to the system, return value is the bundle adjuster's ID for the point
int Bundle::AddPoint(Vector<3> v3Pos, bool bfixed)
{
    int n = mvPoints.size();
    Point p;
    p.bfixed = bfixed;
    if(isnan(v3Pos * v3Pos))
    {
        cerr << " You sucker, tried to give me a nan " << v3Pos << endl;
        v3Pos = Zeros;
    }
    p.v3Pos = v3Pos;
    mvPoints.push_back(p);
    return n;
}

// Add a measurement of one point with one camera
void Bundle::AddMeas(int nCam, int nPoint, Vector<2> v2Pos, double dSigmaSquared, int nCamnum, bool mAssociated)
{
    assert(nCam < (int) mvCameras.size());
    assert(nPoint < (int) mvPoints.size());
    mvPoints[nPoint].nMeasurements++;
    mvPoints[nPoint].sCameras.insert(nCam);
    Meas2d* m = new Meas2d(nPoint, nCam, v2Pos, dSigmaSquared);
    if (nCamnum)
        m->nSourceCamera = nCamnum;
    if (nCamnum&&mAssociated)
        m->mAssociated = true;
    else
        m->mAssociated = false;
//    m->mse3Cam2FromCam1 = mse3Cam2FromCam1;
    mMeasList.push_back(m);
}

// Add a 3D measurement of one point with one camera
void Bundle::AddMeas(int nCam, int nPoint, Vector<3> v3Pos, double dSigmaSquared, int nCamnum)
{
    assert(nCam < (int) mvCameras.size());
    assert(nPoint < (int) mvPoints.size());
    mvPoints[nPoint].nMeasurements++;
    mvPoints[nPoint].sCameras.insert(nCam);
    Meas3d* m = new Meas3d(nPoint, nCam, v3Pos, dSigmaSquared);
    if (nCamnum)
        m->nSourceCamera = nCamnum;
    mMeasList.push_back(m);
}

// Add a depth measurement of one point with one camera
void Bundle::AddMeas(int nCam, int nPoint, double dDepth, double dSigmaSquared, int nCamnum, bool mAssociated)
{
    assert(nCam < (int) mvCameras.size());
    assert(nPoint < (int) mvPoints.size());
    mvPoints[nPoint].nMeasurements++;
    mvPoints[nPoint].sCameras.insert(nCam);
    MeasDepth* m = new MeasDepth(nPoint, nCam, dDepth, dSigmaSquared);
    if (nCamnum)
        m->nSourceCamera = nCamnum;
    if (nCamnum&&mAssociated)
        m->mAssociated = true;
    else
        m->mAssociated = false;
    mMeasList.push_back(m);
}


// Zero temporary quantities stored in cameras and points
void Bundle::ClearAccumulators()
{
    for(size_t i=0; i<mvPoints.size(); ++i)
    {
        mvPoints[i].m3V = Zeros;
        mvPoints[i].v3EpsilonB = Zeros;
    }
    for(size_t i=0; i<mvCameras.size(); ++i)
    {
        mvCameras[i].m6U = Zeros;
        mvCameras[i].v6EpsilonA = Zeros;
    }
}

// Perform bundle adjustment. The parameter points to a signal bool 
// which mapmaker will set to high if another keyframe is incoming
// and bundle adjustment needs to be aborted.
// Returns number of accepted iterations if all good, negative 
// value for big error.
// TODO: add rigid connection between dual keyframes, otherwise, could be used for dual camera calibration
int Bundle::Compute(bool *pbAbortSignal)
{
    cout << "in compute..." << endl;
    mpbAbortSignal = pbAbortSignal;

    // Some speedup data structures
    GenerateMeasLUTs();
    GenerateOffDiagScripts();

    // Initially behave like gauss-newton
    mdLambda = 0.1; //0.0001;
    mdLambdaFactor = 2.0;
    mbConverged = false;
    mbHitMaxIterations = false;
    mnCounter = 0;
    mnAccepted = 0;

    // What MEstimator are we using today?
    static gvar3<string> gvsMEstimator("BundleMEstimator", "Tukey", SILENT);

    cout << "estimating..." << endl;
    while(!mbConverged  && !mbHitMaxIterations && !*pbAbortSignal)
    {
        bool bNoError;
        if(*gvsMEstimator == "Cauchy")
            bNoError = Do_LM_Step<Cauchy>(pbAbortSignal);
        else if(*gvsMEstimator == "Tukey")
            bNoError = Do_LM_Step<Tukey>(pbAbortSignal);
        else if(*gvsMEstimator == "Huber")
            bNoError = Do_LM_Step<Huber>(pbAbortSignal);
        else if (*gvsMEstimator == "LeastSquares")
            bNoError = Do_LM_Step<LeastSquares>(pbAbortSignal);
        else
        {
            cout << "Invalid BundleMEstimator selected !! " << endl;
            cout << "Defaulting to Tukey." << endl;
            *gvsMEstimator = "Tukey";
            bNoError = Do_LM_Step<Tukey>(pbAbortSignal);
        };

        if(!bNoError)
            return -1;
    }

    if(mbHitMaxIterations)
        cout << "  Hit max iterations." << endl;
    cout << "Final Sigma Squared: " << mdSigmaSquared << " (= " << sqrt(mdSigmaSquared) / 4.685 << " pixels.)" << endl;
    return mnAccepted;
};


template<class MEstimator>
bool Bundle::Do_LM_Step(bool *pbAbortSignal)
{
//    cout << "in LBA..." << endl;
    // Reset all accumulators to zero
    ClearAccumulators();

    //  Do a LM step according to Hartley and Zisserman Algo A6.4 in MVG 2nd Edition
    //  Actual work starts a bit further down - first we have to work out the
    //  projections and errors for each point, so we can do tukey reweighting
    vector<double> vdErrorSquared;
    vector<double> vdErrorSquaredDepth;
    for(list<Meas*>::iterator itr = mMeasList.begin(); itr!=mMeasList.end(); itr++)
    {
        Meas* meas = *itr;
        if (!(*itr)->nSourceCamera)
            meas->ProjectAndFindSquaredError(mvPoints,mvCameras,mCamera.get());
        else
            meas->ProjectAndFindSquaredError(mvPoints,mvCameras,mCameraSec[(*itr)->nSourceCamera-1].get());

        if(!meas->bBad){
            if (!meas->bdepth)
                vdErrorSquared.push_back(meas->dErrorSquared);
            else
                vdErrorSquaredDepth.push_back(meas->dErrorSquared);
        }
    };

    // Projected all points and got vector of errors; find the median,
    // And work out robust estimate of sigma, then scale this for the tukey
    // estimator
    mdSigmaSquared = MEstimator::FindSigmaSquared(vdErrorSquared);
    if (vdErrorSquaredDepth.size())
        mdSigmaSquaredDepth = MEstimator::FindSigmaSquared(vdErrorSquaredDepth);

    // Initially the median error might be very small - set a minimum
    // value so that good measurements don't get erased!
    static gvar3<double> gvdMinSigma("Bundle.MinTukeySigma", 0.4, SILENT);
    const double dMinSigmaSquared = *gvdMinSigma * *gvdMinSigma;
    if(mdSigmaSquared < dMinSigmaSquared)
        mdSigmaSquared = dMinSigmaSquared;
    if(mdSigmaSquaredDepth < dMinSigmaSquared)
        mdSigmaSquaredDepth = dMinSigmaSquared;


    //  OK - good to go! weights can now be calced on second run through the loop.
    //  Back to Hartley and Zisserman....
    //  `` (i) Compute the derivative matrices Aij = [dxij/daj]
    //      and Bij = [dxij/dbi] and the error vectors eij''
    //
    //  Here we do this by looping over all measurements
    //
    //  While we're here, might as well update the accumulators U, ea, B, eb
    //  from part (ii) as well, and let's calculate Wij while we're here as well.
    // dual camera PTAM modification: measurements from the second cam kfs with association would have
    // different Jacobian

    double dCurrentError = 0.0;
    for(list<Meas*>::iterator itr = mMeasList.begin(); itr!=mMeasList.end(); itr++)
    {
        Meas* meas = *itr;
        Camera &cam = mvCameras[meas->c];// take care the redirection for dual cam association cases.
        Point &point = mvPoints[meas->p];

        // Project the point.
        // We've done this before - results are still cached in meas.
        if(meas->bBad)
        {
            dCurrentError += 1.0;
            continue;
        };

        // What to do with the weights? The easiest option is to independently weight
        // The two jacobians, A and B, with sqrt of the tukey weight w;
        // And also weight the error vector v2Epsilon.
        // That makes everything else automatic.
        // Calc the square root of the tukey weight:
        double mdsigmaSq;
        if (meas->bdepth)
            mdsigmaSq = mdSigmaSquaredDepth;
        else
            mdsigmaSq = mdSigmaSquared;
        double dWeight = MEstimator::SquareRootWeight(meas->dErrorSquared, mdsigmaSq);
        // Re-weight error:
//        if (meas->nSourceCamera)
//            dWeight = dWeight * 0.6;

        meas->SetWeight(dWeight);

        if(dWeight == 0) {
            meas->bBad = true;
            dCurrentError += 1.0;
            continue;
        }

        dCurrentError += MEstimator::ObjectiveScore(meas->dErrorSquared, mdsigmaSq);

        // To re-weight the jacobians, I'll just re-weight the camera param matrix
        // This is only used for the jacs and will save a few fmuls
        //      Matrix<2> m2CamDerivs = dWeight * meas.m2CamDerivs;
        //      TODO:meas->ReweightJacobians(dWeight);

        //      const double dOneOverCameraZ = 1.0 / meas->v3Cam[2];
        //      const Vector<4> v4Cam = unproject(meas->v3Cam);

        // Calculate A: (the proj derivs WRT the camera)
        if(cam.bFixed) {
            meas->SetAZero();
        } else {
            meas->CalculateA();

        }

        // Calculate B: (the proj derivs WRT the point)
        // yang, fix those map points which had been transfered source kf id in VO case
        if (point.bfixed)
            meas->SetBZero();
        else
            meas->CalculateB(cam);

        // Update the accumulators
        if(!cam.bFixed) {
            //	  cam.m6U += meas.m26A.T() * meas.m26A; 	  // SLOW SLOW this matrix is symmetric
            //BundleTriangle_UpdateM6U_LL(cam.m6U, meas.m26A);
            meas->UpdateAccumulators(cam);
            //          meas->BundleTriangle_UpdateM6U_LL();
            //	  cam.v6EpsilonA += meas.m26A.T() * meas.v2Epsilon;
            // NOISE COVAR OMITTED because it's the 2-Identity
        }

        //            point.m3V += meas.m23B.T() * meas.m23B;             // SLOW-ish this is symmetric too
        // BundleTriangle_UpdateM3V_LL(point.m3V, meas.m23B);
        // point.v3EpsilonB += meas.m23B.T() * meas.v2Epsilon;
        meas->UpdateAccumulators(point);

        if(cam.bFixed)
            meas->SetWZero();
        else
            meas->CalculateW();

    }

//    cout << "BA iterating..." << endl;
    // OK, done (i) and most of (ii) except calcing Yij; this depends on Vi, which should
    // be finished now. So we can find V*i (by adding lambda) and then invert.
    // The next bits depend on mdLambda! So loop this next bit until error goes down.
    double dNewError = dCurrentError + 9999;
    while(dNewError > dCurrentError && !mbConverged && !mbHitMaxIterations && !*pbAbortSignal)
    {
        // Rest of part (ii) : find V*i inverse
        for(vector<Point>::iterator itr = mvPoints.begin(); itr!=mvPoints.end(); itr++)
        {
            Point &point = *itr;
            Matrix<3> m3VStar = point.m3V;
            if(m3VStar[0][0] * m3VStar[1][1] * m3VStar[2][2] == 0)
                point.m3VStarInv = Zeros;
            else
            {
                // Fill in the upper-r triangle from the LL;
                m3VStar[0][1] = m3VStar[1][0];
                m3VStar[0][2] = m3VStar[2][0];
                m3VStar[1][2] = m3VStar[2][1];

                for(int i=0; i<3; i++)
                    m3VStar[i][i] *= (1.0 + mdLambda);
                Cholesky<3> chol(m3VStar);
                point.m3VStarInv = chol.get_inverse();
            };
        }

        // Done part (ii), except calculating Yij;
        // But we can do this inline when we calculate S in part (iii).

        // Part (iii): Construct the the big block-matrix S which will be inverted.
        Matrix<> mS(mnCamsToUpdate * 6, mnCamsToUpdate * 6);
        mS = Zeros;
        Vector<> vE(mnCamsToUpdate * 6);
        vE = Zeros;

        Matrix<6> m6; // Temp working space
        Vector<6> v6; // Temp working space

        // Calculate on-diagonal blocks of S (i.e. only one camera at a time:)
        for(unsigned int j=0; j<mvCameras.size(); j++)
        {
            Camera &cam_j = mvCameras[j];
            if(cam_j.bFixed) continue;
            int nCamJStartRow = cam_j.nStartRow;

            // First, do the diagonal elements.
            //m6= cam_j.m6U;     // can't do this anymore because cam_j.m6U is LL!!
            for(int r=0; r<6; r++)
            {
                for(int c=0; c<r; c++)
                    m6[r][c] = m6[c][r] = cam_j.m6U[r][c];
                m6[r][r] = cam_j.m6U[r][r];
            };

            for(int nn = 0; nn< 6; nn++)
                m6[nn][nn] *= (1.0 + mdLambda);

            v6 = cam_j.v6EpsilonA;

            vector<Meas*> &vMeasLUTj = mvMeasLUTs[j];
            // Sum over measurements (points):
            for(unsigned int i=0; i<mvPoints.size(); i++)
            {
                Meas* pMeas = vMeasLUTj[i];
                if(pMeas == NULL || pMeas->bBad)
                    continue;
                m6 -= pMeas->m63W * mvPoints[i].m3VStarInv * pMeas->m63W.T();  // SLOW SLOW should by 6x6sy
                v6 -= pMeas->m63W * (mvPoints[i].m3VStarInv * mvPoints[i].v3EpsilonB);
            }
            mS.slice(nCamJStartRow, nCamJStartRow, 6, 6) = m6;
            vE.slice(nCamJStartRow,6) = v6;
        }

        // Now find off-diag elements of S. These are camera-point-camera combinations, of which there are lots.
        // New code which doesn't waste as much time finding i-jk pairs; these are pre-stored in a per-i list.
        for(unsigned int i=0; i<mvPoints.size(); i++)
        {
            Point &p = mvPoints[i];
            int nCurrentJ = -1;
            int nJRow = -1;
            Meas* pMeas_ij;
            Meas* pMeas_ik;
            Matrix<6,3> m63_MIJW_times_m3VStarInv;

            for(vector<OffDiagScriptEntry>::iterator it=p.vOffDiagonalScript.begin();
                it!=p.vOffDiagonalScript.end();
                it++)
            {
                OffDiagScriptEntry &e = *it;
                pMeas_ik = mvMeasLUTs[e.k][i];
                if(pMeas_ik == NULL || pMeas_ik->bBad)
                    continue;
                if(e.j != nCurrentJ)
                {
                    pMeas_ij = mvMeasLUTs[e.j][i];
                    if(pMeas_ij == NULL || pMeas_ij->bBad)
                        continue;
                    nCurrentJ = e.j;
                    nJRow = mvCameras[e.j].nStartRow;
                    m63_MIJW_times_m3VStarInv = pMeas_ij->m63W * p.m3VStarInv;
                }
                int nKRow = mvCameras[pMeas_ik->c].nStartRow;
#ifndef WIN32
                mS.slice(nJRow, nKRow, 6, 6) -= m63_MIJW_times_m3VStarInv * pMeas_ik->m63W.T();
#else
                Matrix<6> m = mS.slice(nJRow, nKRow, 6, 6);
                m -= m63_MIJW_times_m3VStarInv * pMeas_ik->m63W.T();
                mS.slice(nJRow, nKRow, 6, 6) = m;
#endif
                assert(nKRow < nJRow);
            }
        }

        // Did this purely LL triangle - now update the TR bit as well!
        // (This is actually unneccessary since the lapack cholesky solver
        // uses only one triangle, but I'm leaving it in here anyway.)
        for(int i=0; i<mS.num_rows(); i++)
            for(int j=0; j<i; j++)
                mS[j][i] = mS[i][j];

        // Got fat matrix S and vector E from part(iii). Now Cholesky-decompose
        // the matrix, and find the camera update vector.
        Vector<> vCamerasUpdate(mS.num_rows());
        vCamerasUpdate = Cholesky<>(mS).backsub(vE);

        // Part (iv): Compute the map updates
        Vector<> vMapUpdates(mvPoints.size() * 3);
        int nan_num = 0;
        for(unsigned int i=0; i<mvPoints.size(); i++)
        {
            Vector<3> v3Sum;
            v3Sum = Zeros;
            for(unsigned int j=0; j<mvCameras.size(); j++)
            {
                Camera &cam = mvCameras[j];
                if(cam.bFixed)
                    continue;
                Meas *pMeas = mvMeasLUTs[j][i];
                if(pMeas == NULL || pMeas->bBad)
                    continue;
                v3Sum+=pMeas->m63W.T() * vCamerasUpdate.slice(cam.nStartRow,6);
            }
            Vector<3> v3 = mvPoints[i].v3EpsilonB - v3Sum;
            vMapUpdates.slice(i * 3, 3) = mvPoints[i].m3VStarInv * v3;
            if(isnan(vMapUpdates.slice(i * 3, 3) * vMapUpdates.slice(i * 3, 3)))
            {
                nan_num ++;
//                cerr << "NANNERY! " << endl;
//                cerr << mvPoints[i].m3VStarInv << endl;
            };
        }
        if (nan_num)
            cerr << "NANNERY POINTS: " << nan_num << endl;

        // OK, got the two update vectors.
        // First check for convergence..
        // (this is a very poor convergence test)
        double dSumSquaredUpdate = vCamerasUpdate * vCamerasUpdate + vMapUpdates * vMapUpdates;
        if(dSumSquaredUpdate< *mgvdUpdateConvergenceLimit)
            mbConverged = true;

        // Now re-project everything and measure the error;
        // NB we don't keep these projections, SLOW, bit of a waste.

        // Temp versions of updated pose and pos:
        for(unsigned int j=0; j<mvCameras.size(); j++)
        {
            if(mvCameras[j].bFixed)
                mvCameras[j].se3CfWNew = mvCameras[j].se3CfW;
            else
                mvCameras[j].se3CfWNew = SE3<>::exp(vCamerasUpdate.slice(mvCameras[j].nStartRow, 6)) * mvCameras[j].se3CfW;
        }
        for(unsigned int i=0; i<mvPoints.size(); i++)
        {
            if (mvPoints[i].bfixed)
                mvPoints[i].v3PosNew = mvPoints[i].v3Pos;
            else
                mvPoints[i].v3PosNew = mvPoints[i].v3Pos + vMapUpdates.slice(i*3, 3);
        }
        // Calculate new error by re-projecting, doing tukey, etc etc:
        dNewError = FindNewError<MEstimator>();

        cout <<setprecision(1) << "L" << mdLambda << setprecision(3) <<  "\tOld " << dCurrentError << "  New " << dNewError << "  Diff " << dCurrentError - dNewError << "\t";

        // Was the step good? If not, modify lambda and try again!!
        // (if it was good, will break from this loop.)
        if(dNewError > dCurrentError)
        {
            cout << " TRY AGAIN " << endl;
            ModifyLambda_BadStep();
        };

        mnCounter++;
        if(mnCounter >= *mgvnMaxIterations)
            mbHitMaxIterations = true;
    }   // End of while error too big loop

    if(dNewError < dCurrentError) // Was the last step a good one?
    {
        cout << " WINNER            ------------ " << endl;
        // Woo! got somewhere. Update lambda and make changes permanent.
        ModifyLambda_GoodStep();
        for(unsigned int j=0; j<mvCameras.size(); j++)
            mvCameras[j].se3CfW = mvCameras[j].se3CfWNew;
        for(unsigned int i=0; i<mvPoints.size(); i++)
            mvPoints[i].v3Pos = mvPoints[i].v3PosNew;
        mnAccepted++;
    }

    // Finally, ditch all the outliers.
    vector<list<Meas*>::iterator> vit;
    for(list<Meas*>::iterator itr = mMeasList.begin(); itr!=mMeasList.end(); itr++) {
        Meas* meas = *itr;
        if(meas->bBad) {
            vit.push_back(itr);
            mvOutlierMeasurementIdx.push_back(make_pair(meas->p, meas->c));
            mvPoints[meas->p].nOutliers++;
            mvMeasLUTs[meas->c][meas->p] = NULL;
        }
    }

    for(unsigned int i=0; i<vit.size(); i++) {
        delete *vit[i];
        mMeasList.erase(vit[i]);
    }

    cout << "Nuked " << vit.size() << " measurements." << endl;
    return true;
}

// Find the new total error if cameras and points used their 
// new coordinates
template<class MEstimator>
double Bundle::FindNewError()
{
    ofstream ofs;
    double dNewError = 0;
    vector<double> vdErrorSquared;
    for(list<Meas*>::iterator itr = mMeasList.begin(); itr!=mMeasList.end(); itr++)
    {
        Meas* meas = *itr;
        // Project the point.
        Vector<3> v3Cam;
        if (!(*itr)->nSourceCamera || !(*itr)->mAssociated)
            v3Cam = mvCameras[meas->c].se3CfWNew * mvPoints[meas->p].v3PosNew;
        else
            v3Cam = (mvCameras[meas->c].se3C2fC1* mvCameras[meas->c].se3CfWNew) * mvPoints[meas->p].v3PosNew;
        if(v3Cam[2] <= 0) {
            dNewError += 1.0;
            cout << ".";
            continue;
        }

        double dErrorSquared;
        if (!meas->nSourceCamera)
            dErrorSquared = meas->EstimateNewSquaredError(v3Cam,mCamera.get());
        else
            dErrorSquared = meas->EstimateNewSquaredError(v3Cam,mCameraSec[meas->nSourceCamera - 1].get());
        dNewError += MEstimator::ObjectiveScore(dErrorSquared, mdSigmaSquared);
    }
    return dNewError;
}

// Optimisation: make a bunch of tables, one per camera
// which point to measurements (if any) for each point
// This is faster than a std::map lookup.
void Bundle::GenerateMeasLUTs()
{
    mvMeasLUTs.clear();
    for(unsigned int nCam = 0; nCam < mvCameras.size(); nCam++)
    {
        mvMeasLUTs.push_back(vector<Meas*>());
        mvMeasLUTs.back().resize(mvPoints.size(), NULL);
    };
    for(list<Meas*>::iterator it = mMeasList.begin(); it!=mMeasList.end(); it++) {
        Meas* meas = *it;
        mvMeasLUTs[meas->c][meas->p] = meas;
    }
}

// Optimisation: make a per-point list of all
// observation camera-camera pairs; this is then
// scanned to make the off-diagonal elements of matrix S
void Bundle::GenerateOffDiagScripts()
{
    for(unsigned int i=0; i<mvPoints.size(); i++)
    {
        Point &p = mvPoints[i];
        p.vOffDiagonalScript.clear();
        for(set<int>::iterator it_j = p.sCameras.begin(); it_j!=p.sCameras.end(); it_j++)
        {
            int j = *it_j;
            if(mvCameras[j].bFixed)
                continue;
            Meas *pMeas_j = mvMeasLUTs[j][i];
            assert(pMeas_j != NULL);

            for(set<int>::iterator it_k = p.sCameras.begin(); it_k!=it_j; it_k++)
            {
                int k = *it_k;
                if(mvCameras[k].bFixed)
                    continue;

                Meas *pMeas_k = mvMeasLUTs[k][i];
                assert(pMeas_k != NULL);

                OffDiagScriptEntry e;
                e.j = j;
                e.k = k;
                p.vOffDiagonalScript.push_back(e);
            }
        }
    }
}

void Bundle::ModifyLambda_GoodStep()
{
    mdLambdaFactor = 2.0;
    mdLambda *= 0.3;
};

void Bundle::ModifyLambda_BadStep()
{
    mdLambda = mdLambda * mdLambdaFactor;
    mdLambdaFactor = mdLambdaFactor * 2;
};


Vector<3> Bundle::GetPoint(int n)
{
    return mvPoints.at(n).v3Pos;
}

SE3<> Bundle::GetCamera(int n)
{
    return mvCameras.at(n).se3CfW;
}

set<int> Bundle::GetOutliers()
{
    set<int> sOutliers;
    set<int>::iterator hint = sOutliers.begin();
    for(unsigned int i=0; i<mvPoints.size(); i++)
    {
        Point &p = mvPoints[i];
        if(p.nMeasurements > 0 && p.nMeasurements == p.nOutliers)
            hint = sOutliers.insert(hint, i);
    }
    return sOutliers;
};


vector<pair<int, int> > Bundle::GetOutlierMeasurements()
{
    return mvOutlierMeasurementIdx;
}









