// pwcarmafiller:  (carma) miriad dataset to MeasurementSet conversion
// Modified by Peter Williams for ATA data
// Copyright 2010-2013
//
// Base code:
// Copyright (C) 1997,2000,2001,2002
// Associated Universities, Inc. Washington DC, USA.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Masve, Cambridge, MA 02139, USA.
//
// $Id: carmafiller.cc,v 1.30 2010/06/23 18:14:08 pteuben Exp $


#include <casa/aips.h>
#include <casa/stdio.h>
#include <casa/iostream.h>
#include <casa/OS/File.h>
#include <casa/Utilities/GenSort.h>

#include <casa/Arrays/Cube.h>
#include <casa/Arrays/Matrix.h>
#include <casa/Arrays/Vector.h>
#include <casa/Arrays/ArrayMath.h>
#include <casa/Arrays/ArrayUtil.h>
#include <casa/Arrays/ArrayLogical.h>
#include <casa/Arrays/MatrixMath.h>

#include <casa/Inputs/Input.h>

#include <measures/Measures.h>
#include <measures/Measures/MPosition.h>
#include <measures/Measures/MeasData.h>
#include <measures/Measures/Stokes.h>

#include <tables/Tables.h>
#include <tables/Tables/TableInfo.h>

#include <ms/MeasurementSets.h>

#include <miriad-c/maxdimc.h>
#include <miriad-c/miriad.h>

#include <casa/namespace.h>

#ifndef MAXFIELD
# define MAXFIELD 256 // TODO: kill this hardcoding.
#endif

// Based off of carmafiller, which came from bimafiller, and before that,
// uvfitsfiller.
//
// The big problem with this program right now is that we really need to make
// two passes through the dataset to build up lists of all of the pol'n
// configs, pointings, etc., before we can actually start writing everything
// out.

typedef struct window {
    // CASA defines everything mid-band, mid-interval
    int    nspect;                   // number of valid windows (<=MAXWIN, typically 16)
    int    nschan[MAXWIN+MAXWIDE];   // number of channels in a window
    int    ischan[MAXWIN+MAXWIDE];   // starting channel of a window (1-based)
    double sdf[MAXWIN+MAXWIDE];      // channel separation
    double sfreq[MAXWIN+MAXWIDE];    // frequency of first channel in window (doppler changes)
    double restfreq[MAXWIN+MAXWIDE]; // rest freq, if appropriate
    char   code[MAXWIN+MAXWIDE];     // code to CASA identification (N, W or S; S not used anymore)
    int    keep[MAXWIN+MAXWIDE];     // keep this window for output to MS (0=false 1=true)

    int    nwide;                    // number of wide band channels
    float  wfreq[MAXWIDE];           // freq
    float  wwidth[MAXWIDE];          // width
} WINDOW;

class CarmaFiller {
public:
    CarmaFiller (String& infile, Int debug_level=0, Bool apply_tsys=False);

    void checkInput ();
    void setupMeasurementSet (const String& MSFileName);
    void fillObsTables ();
    void fillMSMainTable (Bool scan, Int snumbase);
    void fillAntennaTable ();
    void fillSyscalTable ();
    void fillSpectralWindowTable ();
    void fillFieldTable ();
    void fillSourceTable ();
    void fillFeedTable ();
    void fixEpochReferences ();

private:
    void Tracking (int record);
    void init_window ();

    bool uv_hasvar (const char *varname);
    char *uv_getstr (const char *varname);
    int uv_getint (const char *varname);

    String infile_p;
    Int uv_handle_p;
    MeasurementSet ms_p;
    MSColumns *msc_p;
    Int debug_level;
    Int nIF_p;
    String telescope_name, project_p, object_p, telescope_p,
	observer_name, timsys_p;
    Vector<Int> nPixel_p, corrType_p, corrIndex_p;
    Matrix<Int> corrProduct_p;
    Double epoch_p;
    MDirection::Types epochRef_p;
    Int nArray_p;
    Block<Int> nAnt_p;
    Block<Vector<Double> > receptorAngle_p;
    Vector<Double> arrayXYZ_p; // 3 elements
    Vector<Double> ras_p, decs_p; // ra/dec for source list (source_p)
    Vector<String> source_p, purpose_p; // list of source names (?? object_p ??)

    // the following variables are for miriad, hence not Double/Int/Float

    double preamble[5], first_time;
    int ifield, nfield, npoint, nsource;     // both dra/ddec should become Vector's
    float dra[MAXFIELD], ddec[MAXFIELD];       // offset in radians
    double ra[MAXFIELD], dec[MAXFIELD];
    int field[MAXFIELD];                     // source index
    int fcount[MAXFIELD], sid_p[MAXFIELD];
    float dra_p, ddec_p;
    int pol_p;

    Vector<Int> polmapping;
    Int nants_p, nchan_p, nwide_p, npol_p;
    Double antpos[3*MAXANT];
    double longitude;
    Double ra_p, dec_p;       // current pointing center RA,DEC at EPOCH
    Float inttime_p;
    Double freq_p;            // rest frequency of the primary line
    Int mount_p;
    Double time_p;            // current MJD time

    WINDOW win;
    Bool apply_tsys;    /* tsys weights */

    float data[2*MAXCHAN], wdata[2*MAXCHAN];	// 2*MAXCHAN since (Re,Im) pairs complex numbers
    int flags[MAXCHAN], wflags[MAXCHAN];
    float systemp[MAXANT*MAXWIDE];
    int zero_tsys;
    int nvis;
};


CarmaFiller::CarmaFiller (String& infile, Int debug_level, Bool apply_tsys)
{
    infile_p = infile;
    nArray_p = 0;
    nfield = 0;
    npoint = 0;
    this->debug_level = debug_level;
    this->apply_tsys = apply_tsys;
    zero_tsys = 0;

    for (int i = 0; i < MAXFIELD; i++)
	fcount[i] = 0;

    if (sizeof (double) != sizeof (Double))
	cout << "Double != double; carmafiller will probably fail" << endl;
    if (sizeof (int) != sizeof (Int))
	cout << "int != Int; carmafiller will probably fail" << endl;

    uvopen_c (&uv_handle_p, infile_p.chars (), "old");
    uvset_c (uv_handle_p, "preamble", "uvw/time/baseline", 0, 0.0, 0.0, 0.0);
    Tracking (-1);
}


#define DEBUG(level) (this->debug_level >= (level))
#define WARN(message) (cerr << "warning: " << message << endl);

bool
CarmaFiller::uv_hasvar (const char *varname)
{
    /* Also tests whether the variable has been updated if it's being tracked. */
    int vupd, vlen;
    char vtype[10];

    uvprobvr_c (uv_handle_p, varname, vtype, &vlen, &vupd);
    return vupd;
}


char *
CarmaFiller::uv_getstr (const char *varname)
{
    char *value = new char[64];
    // note: can't use sizeof(*value) since the size parameter is an int. Boo.
    uvgetvr_c (uv_handle_p, H_BYTE, varname, value, 64);
    return value;
}


int
CarmaFiller::uv_getint (const char *varname)
{
    int value;
    uvgetvr_c (uv_handle_p, H_INT, varname, (char *) &value, 1);
    // XXX: error checking!
    return value;
}


void
CarmaFiller::checkInput ()
{
    Int i, nread, nwread;
    Float epoch;

    uvread_c (uv_handle_p, preamble, data, flags, MAXCHAN, &nread);
    uvwread_c (uv_handle_p, wdata, wflags, MAXCHAN, &nwread);
    if (nread <= 0 && nwread <= 0)
	throw AipsError ("no UV data present");
    init_window ();

    if (win.nspect > 0) {
	nchan_p = nread;
	nwide_p = nwread;
    } else {
	nchan_p = nread;
	nwide_p = 0;
    }

    // Get the initial array configuration
    nants_p = uv_getint ("nants");
    uvgetvr_c (uv_handle_p, H_DBLE, "antpos", (char *) antpos, 3 * nants_p);
    uvgetvr_c (uv_handle_p, H_DBLE, "longitu", (char *) &longitude, 1);

    // Note: systemp is stored systemp[nants][nwin] in C notation
    if (win.nspect > 0)
	uvgetvr_c (uv_handle_p, H_REAL, "systemp", (char *) systemp, nants_p * win.nspect);
    else
	uvgetvr_c (uv_handle_p, H_REAL, "wsystemp", (char *) systemp, nants_p);

    if (win.nspect > 0)
	uvgetvr_c (uv_handle_p, H_DBLE, "restfreq", (char *)win.restfreq, win.nspect);


    if (uv_hasvar ("project"))
	project_p = uv_getstr ("project");
    else
	project_p = "unknown";

    object_p = uv_getstr ("source");
    telescope_name = uv_getstr ("telescop");

    if (uv_hasvar ("observer"))
	observer_name = uv_getstr ("observer");
    else
	observer_name = "unknown";

    mount_p = 0;

    uvgetvr_c (uv_handle_p, H_REAL, "epoch", (char *) &epoch, 1);
    epoch_p = epoch;
    epochRef_p = MDirection::J2000;
    if (nearAbs (epoch_p, 1950.0, 0.01))
	epochRef_p = MDirection::B1950;

    // TODO: these should all be handled on-the-fly.
    npol_p = uv_getint ("npol");
    pol_p = uv_getint ("pol");
    uvgetvr_c (uv_handle_p, H_REAL, "inttime", (char *) &inttime_p, 1);
    uvgetvr_c (uv_handle_p, H_DBLE, "freq", (char *) &freq_p, 1);
    freq_p *= 1e9; // GHz -> Hz

    uvgetvr_c (uv_handle_p, H_DBLE, "ra", (char *) &ra_p, 1);
    uvgetvr_c (uv_handle_p, H_DBLE, "dec", (char *) &dec_p, 1);

    if (hexists_c (uv_handle_p, "gains"))
	WARN ("a gains table is present, but this tool cannot apply them");
    if (hexists_c (uv_handle_p, "bandpass"))
	WARN ("a bandpass table is present, but this tool cannot apply them");
    if (hexists_c (uv_handle_p, "leakage"))
	WARN ("a leakage table is present, but this tool cannot apply them");

    uvrewind_c (uv_handle_p);

    // XXX: hardcoding assumption of full-Stokes XY pol
    npol_p = 4;
    corrType_p.resize (npol_p);
    corrType_p(0) = Stokes::XX;
    corrType_p(1) = Stokes::XY;
    corrType_p(2) = Stokes::YX;
    corrType_p(3) = Stokes::YY;
    polmapping.resize (13);
    polmapping = -1;
    polmapping(-5 + 8) = 0;
    polmapping(-6 + 8) = 3;
    polmapping(-7 + 8) = 1;
    polmapping(-8 + 8) = 2;

    corrProduct_p.resize (2, npol_p);
    corrProduct_p = 0;

    for (i = 0; i < npol_p; i++) {
	Fallible<Int> receptor = Stokes::receptor1 (Stokes::type (corrType_p(i)));
	if (receptor.isValid ())
	    corrProduct_p(0,i) = receptor;

	receptor = Stokes::receptor2 (Stokes::type (corrType_p(i)));
	if (receptor.isValid ())
	    corrProduct_p(1,i) = receptor;
    }
}


void CarmaFiller::setupMeasurementSet(const String& MSFileName)
{
  if (DEBUG(1)) cout << "CarmaFiller::setupMeasurementSet" << endl;

  Int nCorr = npol_p;   // STOKES axis
  Int nChan = nchan_p;  // we are only exporting the narrow channels to the MS

  nIF_p = win.nspect;   // number of spectral windows (for narrow channels only)

  // Make the MS table
  TableDesc td = MS::requiredTableDesc();

  MS::addColumnToDesc(td, MS::DATA,2);
  td.removeColumn(MS::columnName(MS::FLAG));
  MS::addColumnToDesc(td, MS::FLAG,2);

  td.defineHypercolumn("TiledData",3,
		       stringToVector(MS::columnName(MS::DATA)));
  td.defineHypercolumn("TiledFlag",3,
		       stringToVector(MS::columnName(MS::FLAG)));
  td.defineHypercolumn("TiledUVW",2,
		       stringToVector(MS::columnName(MS::UVW)));

  if (DEBUG(1))  cout << "Creating MS=" << MSFileName  << endl;
  SetupNewTable newtab(MSFileName, td, Table::New);

  // Set the default Storage Manager to be the Incr one
  IncrementalStMan incrStMan ("ISMData");;
  newtab.bindAll(incrStMan, True);
  // StManAipsIO aipsStMan;  // don't use this anymore
  StandardStMan aipsStMan;  // these are more efficient now


  Int tileSize = nChan / 10 + 1;

  TiledShapeStMan tiledStMan1("TiledData",
			      IPosition(3,nCorr,tileSize,
					16384/nCorr/tileSize));
  TiledShapeStMan tiledStMan1f("TiledFlag",
			       IPosition(3,nCorr,tileSize,
					 16384/nCorr/tileSize));
  TiledShapeStMan tiledStMan2("TiledWeight",
			      IPosition(3,nCorr,tileSize,
					16384/nCorr/tileSize));
  TiledColumnStMan tiledStMan3("TiledUVW",
			       IPosition(2,3,1024));

  // Bind the DATA and FLAG columns to the tiled stman
  newtab.bindColumn(MS::columnName(MS::DATA),tiledStMan1);
  newtab.bindColumn(MS::columnName(MS::FLAG),tiledStMan1f);
  newtab.bindColumn(MS::columnName(MS::UVW),tiledStMan3);

  TableLock lock(TableLock::PermanentLocking);
  MeasurementSet ms(newtab,lock);

  // create all subtables
  // we make new tables with 0 rows
  Table::TableOption option=Table::New;

  // Set up the default subtables for the MS
  ms.createDefaultSubtables(option);

  // Add some optional columns to the required tables
  ms.spectralWindow().addColumn(ArrayColumnDesc<Int>(
    MSSpectralWindow::columnName(MSSpectralWindow::ASSOC_SPW_ID),
    MSSpectralWindow::columnStandardComment(MSSpectralWindow::ASSOC_SPW_ID)));

  ms.spectralWindow().addColumn(ArrayColumnDesc<String>(
    MSSpectralWindow::columnName(MSSpectralWindow::ASSOC_NATURE),
    MSSpectralWindow::columnStandardComment(MSSpectralWindow::ASSOC_NATURE)));

  ms.spectralWindow().addColumn(ScalarColumnDesc<Int>(
    MSSpectralWindow::columnName(MSSpectralWindow::DOPPLER_ID),
    MSSpectralWindow::columnStandardComment(MSSpectralWindow::DOPPLER_ID)));

  // Now setup some optional columns::

  // the SOURCE table, 1 extra optional column needed
  TableDesc sourceDesc = MSSource::requiredTableDesc();
  MSSource::addColumnToDesc(sourceDesc,MSSourceEnums::REST_FREQUENCY,1);
  SetupNewTable sourceSetup(ms.sourceTableName(),sourceDesc,option);
  ms.rwKeywordSet().defineTable(MS::keywordName(MS::SOURCE),
                                     Table(sourceSetup));

  // the DOPPLER table, no optional columns needed
  TableDesc dopplerDesc = MSDoppler::requiredTableDesc();
  SetupNewTable dopplerSetup(ms.dopplerTableName(),dopplerDesc,option);
  ms.rwKeywordSet().defineTable(MS::keywordName(MS::DOPPLER),
                                     Table(dopplerSetup));

  // the SYSCAL table, 1 optional column needed
  TableDesc syscalDesc = MSSysCal::requiredTableDesc();
  MSSysCal::addColumnToDesc(syscalDesc,MSSysCalEnums::TSYS,1);
  SetupNewTable syscalSetup(ms.sysCalTableName(),syscalDesc,option);
  ms.rwKeywordSet().defineTable(MS::keywordName(MS::SYSCAL),
                                     Table(syscalSetup));

  // update the references to the subtable keywords
  ms.initRefs();

  { // Set the TableInfo
    TableInfo& info(ms.tableInfo());
    info.setType(TableInfo::type(TableInfo::MEASUREMENTSET));
    info.setSubType(String("MIRIAD/CARMA"));
    info.readmeAddLine("Made with CarmaFiller");
  }

  ms_p=ms;
  msc_p = new MSColumns(ms_p);
} // setupMeasurementSet()


#define HISTLINE 8192
void CarmaFiller::fillObsTables()
{
  if (DEBUG(1)) cout << "CarmaFiller::fillObsTables" << endl;

  char hline[HISTLINE];
  Int heof;

  ms_p.observation().addRow();
  MSObservationColumns msObsCol(ms_p.observation());

  msObsCol.telescopeName ().put (0, telescope_name);
  msObsCol.observer ().put (0, observer_name);
  msObsCol.project().put(0,project_p);

  MSHistoryColumns msHisCol(ms_p.history());

  String history;
  Int row=-1;
  hisopen_c(uv_handle_p,"read");
  for (;;) {
    hisread_c(uv_handle_p,hline,HISTLINE,&heof);
    if (heof) break;
    ms_p.history().addRow();
    row++;
    msHisCol.observationId().put(row,0);
    //    msHisCol.time().put(row,time);    // fix the "2000/01/01/24:00:00" bug
    //  nono, better file a report, it appears to be an aips++ problem
    msHisCol.priority().put(row,"NORMAL");
    msHisCol.origin().put(row,"CarmaFiller::fillObsTables");
    msHisCol.application().put(row,"carmafiller");
    Vector<String> clicmd (0);
    msHisCol.cliCommand().put(row, clicmd);
    msHisCol.message().put(row,hline);
  }
  hisclose_c(uv_handle_p);
}


void CarmaFiller::fillMSMainTable(Bool scan, Int snumbase)
{
  if (DEBUG(1)) cout << "CarmaFiller::fillMSMainTable" << endl;

  MSColumns& msc(*msc_p);           // Get access to the MS columns, new way
  Int nCorr = npol_p;               // # stokes
  Int nChan = nchan_p;              // # channels to be written
  Int nCat  = 3;                    // # initial flagging categories (fixed at 3)
  Int iscan = snumbase;
  Int ifield_old;

  Matrix<Complex> vis(nCorr,nChan);
  Vector<Float>   sigma(nCorr);
  Vector<String>  cat(nCat);
  cat(0)="FLAG_CMD";
  cat(1)="ORIGINAL";
  cat(2)="USER";
  msc.flagCategory().rwKeywordSet().define("CATEGORY",cat);
  Cube<Bool> flagCat(nCorr,nChan,nCat,False);
  Matrix<Bool> flag = flagCat.xyPlane(0); // references flagCat's storage
  Vector<Float> w1(nCorr), w2(nCorr);

  uvrewind_c(uv_handle_p);

  nAnt_p.resize(1);
  nAnt_p[0]=0;

  receptorAngle_p.resize(1);
  Int group, row=-1;
  int polsleft = 0;
  Double interval;
  Bool lastRowFlag = False;

  if (DEBUG(1))
      cout << "Writing " << nIF_p << " spectral windows" << endl;

  int nread, nwread;
  Int ant1, ant2;
  Float baseline;
  Double time;
  Vector<Double> uvw(3);

  for (group=0; ; group++) {        // loop forever until end-of-file
    uvread_c(uv_handle_p, preamble, data, flags, MAXCHAN, &nread);
    // cout << "UVREAD: " << data[0] << " " << data[1] << endl;
    if (nread <= 0) break;          // done with reading miriad data

    if (DEBUG(9)) cout << "UVREAD: " << nread << endl;
    if (win.nspect > 0)
	uvwread_c(uv_handle_p, wdata, wflags, MAXCHAN, &nwread);
    else
        nwread=0;

    if (nread != nchan_p) {     // big trouble: data width has changed
      cout << "### Error: Narrow Channel changing from " << nchan_p <<
              " to " << nread << endl;
      break;                    // bail out for now
    }
    if (nwread != nwide_p) {     // big trouble: data width has changed
      cout << "### Error: Wide Channel changing from " << nwide_p <<
              " to " << nwread << endl;
      break;                    // bail out for now
    }

    if (polsleft == 0) {
	// starting a new simultaneous polarization record
	uvrdvr_c (uv_handle_p, H_INT, "npol", (char *) &polsleft, NULL, 1);

	baseline = preamble[4];
	ant1 = Int(baseline)/256;              // baseline = 256*A1 + A2
	ant2 = Int(baseline) - ant1*256;       // mostly A1 <= A2

	// get time in MJD seconds ; input was in JD
	time = (preamble[3] - 2400000.5) * C::day;
	time_p = time;

	if (DEBUG(3)) {                 // timeline monitoring...
	    static Double time0 = -1.0;
	    static Double dt0 = -1.0;

	    MVTime mjd_date(time/C::day);
	    mjd_date.setFormat(MVTime::FITS);
	    cout << "DATE=" << mjd_date ;
	    if (time0 > 0) {
		if (time - time0 < 0) {
		    cout << " BACKWARDS";
		    dt0 = time - time0;
		}
	    }
	    if (dt0 > 0) {
		if ( (time-time0) > 5*dt0) {
		    cout << " FORWARDS";
		    dt0 = time - time0;
		}
	    } else
		dt0 = time-time0;
	    time0 = time;
	    cout << endl;
	} // DEBUG(3) for timeline monitoring

	interval = inttime_p;

	// for MIRIAD, this would always cause a single array dataset,
	// but we need to count the antpos occurences to find out
	// which array configuration we're in.

	if (uvupdate_c(uv_handle_p)) {       // aha, something important changed
	    if (DEBUG(4)) {
		cout << "Record " << group+1 << " uvupdate" << endl;
	    }
	    Tracking(group);
	} else {
	    if (DEBUG(5)) cout << "Record " << group << endl;
	}

	//  nAnt_p.resize(array+1);
	//  receptorAngle_p.resize(array+1);

	nAnt_p[nArray_p-1] = max(nAnt_p[nArray_p-1],ant1);   // for MIRIAD, and also
	nAnt_p[nArray_p-1] = max(nAnt_p[nArray_p-1],ant2);
	ant1--; ant2--;                                      // make them 0-based for CASA

	// should ant1 and ant2 be offset with (nArray_p-1)*nant_p ???
	// in case there are multiple arrays???
	// TODO: code should just assuming single array

	uvw(0) = -preamble[0] * 1e-9; // convert to seconds
	uvw(1) = -preamble[1] * 1e-9; // MIRIAD uses nanosec
	uvw(2) = -preamble[2] * 1e-9; // note - sign (CASA vs. MIRIAD convention)
	uvw   *= C::c;                // Finally convert to meters for CASA

	if (group==0 && DEBUG(1)) {
	    cout << "### First record: " << endl;
	    cout << "### Preamble: " << preamble[0] << " " <<
		preamble[1] << " " <<
		preamble[2] << " nanosec.(MIRIAD convention)" << endl;
	    cout << "### uvw: " << uvw(0) << " " <<
		uvw(1) << " " <<
		uvw(2) << " meter. (CASA convention)" << endl;
	}

	flag = 1; // clear all, in case current npol != nCorr
	vis = 0;
    }

    int mirpol;
    Int casapolidx;
    uvrdvr_c (uv_handle_p, H_INT, "pol", (char *) &mirpol, NULL, 1);
    casapolidx = polmapping (mirpol + 8);

    if (casapolidx < 0)
	throw AipsError ("CarmaFiller: unexpected MIRIAD polarization " + mirpol);

    // first construct the data (vis & flag) in a single long array
    // containing all spectral windows
    // In the (optional) loop over all spectral windows, subsets of
    // these arrays will be written out

    Int count = 0;                // index into data[] array

    for (Int chan=0; chan<nChan; chan++) {
	// miriad uses bl=ant1-ant2, FITS/AIPS/CASA use bl=ant2-ant1
	// apart from using -(UVW)'s, the visib need to be conjugated as well
	Bool  visFlag =  (flags[count/2] == 0) ? False : True;
	Float visReal = +data[count]; count++;
	Float visImag = -data[count]; count++;
	Float wt = 1.0;
	if (!visFlag) wt = -wt;

	// check flags array !! need separate counter (count/2)

	flag(casapolidx,chan) = (wt<=0);
	vis(casapolidx,chan) = Complex(visReal,visImag);
    } // chan

    polsleft--;

    if (polsleft == 0 && !allTrue (flag)) {
	// done with this set of simultaneous pols, and not all flagged.

	for (Int ifno=0; ifno < nIF_p; ifno++) {
	    if (win.keep[ifno]==0) continue;
	    // IFs go to separate rows in the MS, pol's do not!
	    ms_p.addRow();
	    row++;

	    // first fill in values for all the unused columns
	    if (row==0) {
		ifield_old = ifield;
		msc.feed1().put(row,0);
		msc.feed2().put(row,0);
		msc.flagRow().put(row,False);
		lastRowFlag = False;
		msc.scanNumber().put(row,iscan);
		msc.processorId().put(row,-1);
		msc.observationId().put(row,0);
		msc.stateId().put(row,-1);
		if (!apply_tsys) {
		    Vector<Float> tmp(nCorr);
		    tmp = 1.0;
		    msc.weight ().put (row, tmp);
		    msc.sigma ().put (row, tmp);
		}
	    }
	    msc.exposure().put(row,interval);
	    msc.interval().put(row,interval);

	    // the dumb way: e.g. 3" -> 20" for 3c273
	    Matrix<Complex> tvis(nCorr,win.nschan[ifno]);
	    Cube<Bool> tflagCat(nCorr,win.nschan[ifno],nCat,False);
	    Matrix<Bool> tflag = tflagCat.xyPlane(0); // references flagCat's storage

	    Int woffset = win.ischan[ifno]-1;
	    Int wsize   = win.nschan[ifno];
	    for (int j = 0; j < nCorr; j++) {
		for (Int i=0; i< wsize; i++) {
		    tvis(j,i) = vis(j,i+woffset);
		    tflag(j,i) = flag(j,i+woffset);
		}
	    }

	    msc.data().put(row,tvis);
	    msc.flag().put(row,tflag);
	    msc.flagCategory().put(row,tflagCat);

	    Bool rowFlag = allEQ(flag,True);
	    if (rowFlag != lastRowFlag) {
		msc.flagRow().put(row,rowFlag);
		lastRowFlag = rowFlag;
	    }

	    msc.antenna1().put(row,ant1);
	    msc.antenna2().put(row,ant2);
	    msc.time().put(row,time);           // CARMA did begin of scan.., now middle (2009)
	    msc.timeCentroid().put(row,time);   // do we really need this ? flagging/blanking ?

	    if (apply_tsys) {
		w2 = 1.0; // "i use this as a 'version' id  to test FC refresh bugs :-)"

		if (systemp[ant1] == 0 || systemp[ant2] == 0) {
		    zero_tsys++;
		    w1 = 0.0;
		} else
		    w1 = 1.0 / sqrt ((double) (systemp[ant1]*systemp[ant2]));

		msc.weight ().put (row, w1);
		msc.sigma ().put (row, w2);
	    }

	    msc.uvw().put(row,uvw);
	    msc.arrayId().put(row,nArray_p-1);
	    msc.dataDescId().put(row,ifno);
	    msc.fieldId().put(row,ifield);

	    // TODO: SCAN_NUMBER needs to be added, they are all 0 now
	    if (ifield_old != ifield)
		iscan++;
	    ifield_old = ifield;
	    msc.scanNumber().put(row,iscan);
	}  // ifNo

	fcount[ifield]++;
    }

  } // for(grou) : loop over all visibilities

  cout << infile_p << ": Processed " << group << " visibilities."
       << endl;
  cout << "Found " << npoint << " pointings with "
       <<  nfield << " unique source/fields "
       <<  source_p.nelements() << " sources and "
       <<  nArray_p << " arrays."
       << endl;
  if (DEBUG(1))
    cout << "nAnt_p contains: " << nAnt_p.nelements() << endl;

}


void CarmaFiller::fillAntennaTable()
{
  if (DEBUG(1)) cout << "CarmaFiller::fillAntennaTable" << endl;
  Int nAnt=nants_p;

  arrayXYZ_p.resize(3);
  if (telescope_name == "HATCREEK" || telescope_name == "BIMA") {     // Array center:
    arrayXYZ_p(0) = -2523862.04;
    arrayXYZ_p(1) = -4123592.80;
    arrayXYZ_p(2) =  4147750.37;
  } else if (telescope_name == "ATA") {
      // ie same as hatcreek / bima -- correct ?????
    arrayXYZ_p(0) = -2523862.04;
    arrayXYZ_p(1) = -4123592.80;
    arrayXYZ_p(2) =  4147750.37;
  } else if (telescope_name == "ATCA") {
    arrayXYZ_p(0) = -4750915.84;
    arrayXYZ_p(1) =  2792906.18;
    arrayXYZ_p(2) = -3200483.75;
  } else if (telescope_name == "OVRO" || telescope_name == "CARMA") {
    arrayXYZ_p(0) = -2397389.65197;
    arrayXYZ_p(1) = -4482068.56252;
    arrayXYZ_p(2) =  3843528.41479;
  } else {
      WARN ("no hardcoded array position available for name " << telescope_name);
      arrayXYZ_p = 0.0;
  }
  if(DEBUG(3)) cout << "number of antennas ="<<nAnt<<endl;
  if(DEBUG(3)) cout << "array ref pos:"<<arrayXYZ_p<<endl;

  String timsys = "TAI";  // assume, for now ....

  // store the time keywords ; again, miriad doesn't have this (yet)
  // check w/ uvfitsfiller again

  //save value to set time reference frame elsewhere
  timsys_p=timsys;

  // Antenna diamater:
  // Should check the 'antdiam' UV variable, but it doesn't appear to
  // exist in our CARMA datasets.
  // So, fill in some likely values
  Float diameter=25;                        //# most common size (:-)
  if (telescope_name=="ATCA")     diameter=22;     //# only at 'low' freq !!
  if (telescope_name=="HATCREEK") diameter=6;
  if (telescope_name=="BIMA")     diameter=6;
  if (telescope_name=="ATA")      diameter=6.1;
  if (telescope_name=="CARMA")    diameter=8;
  if (telescope_name=="OVRO")     diameter=10;

  if (nAnt == 15 && telescope_name=="OVRO") {
    cout << "CARMA array (6 OVRO, 9 BIMA) assumed" << endl;
    telescope_name = "CARMA";
  } else  if (nAnt == 23 && telescope_name=="OVRO") {
    cout << "CARMA array (6 OVRO, 9 BIMA, 8 SZA) assumed" << endl;
    telescope_name = "CARMA";
  }

  Matrix<Double> posRot = Rot3D (2, longitude);

  MSAntennaColumns& ant(msc_p->antenna());
  Vector<Double> antXYZ(3);

  // add antenna info to table
  if (nArray_p == 0) {                   // check if needed
    ant.setPositionRef(MPosition::ITRF);
    //ant.setPositionRef(MPosition::WGS84);
  }
  Int row=ms_p.antenna().nrow()-1;

  if (DEBUG(2)) cout << "CarmaFiller::fillAntennaTable row=" << row+1
       << " array " << nArray_p+1 << endl;

  for (Int i=0; i<nAnt; i++) {

    ms_p.antenna().addRow();
    row++;

    ant.dishDiameter().put(row, diameter);

    antXYZ(0) = antpos[i];              //# these are now in nano-sec
    antXYZ(1) = antpos[i+nAnt];
    antXYZ(2) = antpos[i+nAnt*2];
    antXYZ *= 1e-9 * C::c;;             //# and now in meters
    if (DEBUG(2)) cout << "Ant " << i+1 << ":" << antXYZ << " (m)." << endl;

    String mount;                           // really should consult
    switch (mount_p) {                 	    // the "mount" uv-variable
      case  0: mount="ALT-AZ";      break;
      case  1: mount="EQUATORIAL";  break;
      case  2: mount="X-Y";         break;
      case  3: mount="ORBITING";    break;
      case  4: mount="BIZARRE";     break;
      // case  5: mount="SPACE-HALCA"; break;
      default: mount="UNKNOWN";     break;
    }
    ant.mount().put(row,mount);
    ant.flagRow().put(row,False);
    ant.name().put(row,String::toString(i+1));
    ant.station().put(row,"ANT" + String::toString(i+1));
    ant.type().put(row,"GROUND-BASED");

    Vector<Double> offsets(3);
    offsets=0.0;
    // store absolute positions, with all offsets 0

    antXYZ = product(posRot,antXYZ);
    ant.position().put(row, antXYZ + arrayXYZ_p);
    ant.offset().put(row,offsets);
  }

  nArray_p++;
  nAnt_p.resize(nArray_p);
  nAnt_p[nArray_p-1] = 0;
  if (DEBUG(3) && nArray_p > 1)
    cout << "DEBUG0 :: " << nAnt_p[nArray_p-2] << endl;

  if (nArray_p > 1) return;

  // now do some things which only need to happen the first time around

  // store these items in non-standard keywords for now
  ant.name ().rwKeywordSet ().define ("ARRAY_NAME", telescope_name);
  ant.position().rwKeywordSet().define("ARRAY_POSITION",arrayXYZ_p);
}


void CarmaFiller::fillSyscalTable()
{
  if (DEBUG(1)) cout << "CarmaFiller::fillSyscalTable" << endl;

  MSSysCalColumns&     msSys(msc_p->sysCal());
  Vector<Float> Systemp(1);    // should we set both receptors same?
  static Int row = -1;

  if (DEBUG(1))
    for (Int i=0; i<nants_p; i++)
      cout  << "SYSTEMP: " << i << ": " << systemp[i] << endl;


  for (Int i=0; i<nants_p; i++) {
    ms_p.sysCal().addRow();
    row++;  // should be a static, since this routine will be called again

    msSys.antennaId().put(row,i);
    msSys.feedId().put(row,0);
    msSys.spectralWindowId().put(row,-1);    // all of them for now .....
    msSys.time().put(row,time_p);
    msSys.interval().put(row,-1.0);

    Systemp(0) = systemp[i];
    msSys.tsys().put(row,Systemp);
  }
}


void CarmaFiller::fillSpectralWindowTable()
{
  if (DEBUG(1)) cout << "CarmaFiller::fillSpectralWindowTable" << endl;

  MSSpWindowColumns&      msSpW(msc_p->spectralWindow());
  MSDataDescColumns&      msDD(msc_p->dataDescription());
  MSPolarizationColumns&  msPol(msc_p->polarization());
  MSDopplerColumns&       msDop(msc_p->doppler());

  Int nCorr = npol_p;
  Int i, j, side;
  Double BW = 0.0;

  MDirection::Types dirtype = epochRef_p;    // MDirection::B1950 or MDirection::J2000;
  MEpoch ep(Quantity(time_p, "s"), MEpoch::UTC);
  // ERROR::   type specifier omitted for parameter  in older AIPS++, now works in CASA
  MPosition obspos(MVPosition(arrayXYZ_p), MPosition::ITRF);
  //MPosition obspos(MVPosition(arrayXYZ_p), MPosition::WGS84);
  MDirection dir(Quantity(ra_p, "rad"), Quantity(dec_p, "rad"), dirtype);
  MeasFrame frame(ep, obspos, dir);

  MFrequency::Types freqsys_p = MFrequency::LSRK;

  MFrequency::Convert tolsr(MFrequency::TOPO,
			    MFrequency::Ref(freqsys_p, frame));
  // fill out the polarization info (only 1 entry allowed for now)
  ms_p.polarization().addRow();
  msPol.numCorr().put(0,nCorr);
  msPol.corrType().put(0,corrType_p);
  msPol.corrProduct().put(0,corrProduct_p);
  msPol.flagRow().put(0,False);

  // fill out doppler table (only 1 entry needed, CARMA data only identify 1 line :-(
  for (i=0; i<win.nspect; i++) {
    ms_p.doppler().addRow();
    msDop.dopplerId().put(i,i);
    msDop.sourceId().put(i,-1);     // how the heck..... for all i guess
    msDop.transitionId().put(i,-1);
    msDop.velDefMeas().put(i,MDoppler(Quantity(0),MDoppler::RADIO));
  }

  for (i=0; i < win.nspect; i++)
    {

    Int n = win.nschan[i];
    Vector<Double> f(n), w(n);

    ms_p.spectralWindow().addRow();
    ms_p.dataDescription().addRow();

    msDD.spectralWindowId().put(i,i);
    msDD.polarizationId().put(i,0);
    msDD.flagRow().put(i,False);

    msSpW.numChan().put(i,win.nschan[i]);
    BW = 0.0;
    Double fwin = win.sfreq[i]*1e9;
    if (DEBUG(1)) cout << "Fwin: OBS=" << fwin/1e9;
    fwin = tolsr(fwin).getValue().getValue();
    if (DEBUG(1)) cout << " LSR=" << fwin/1e9 << endl;
    for (j=0; j < win.nschan[i]; j++) {
      f(j) = fwin + j * win.sdf[i] * 1e9;
      w(j) = abs(win.sdf[i]*1e9);
      BW += w(j);
    }

    msSpW.chanFreq().put(i,f);
    if (i<win.nspect)
      msSpW.refFrequency().put(i,win.restfreq[i]*1e9);
    else
      msSpW.refFrequency().put(i,freq_p);            // no reference for wide band???

    msSpW.resolution().put(i,w);
    msSpW.chanWidth().put(i,w);
    msSpW.effectiveBW().put(i,w);
    msSpW.totalBandwidth().put(i,BW);
    msSpW.ifConvChain().put(i,0);
    // can also do it implicitly via Measures you give to the freq's
    msSpW.measFreqRef().put(i,freqsys_p);
    if (i<win.nspect)
      msSpW.dopplerId().put(i,i);    // CARMA has only 1 ref freq line
    else
      msSpW.dopplerId().put(i,-1);    // no ref

    if (win.sdf[i] > 0)      side = 1;
    else if (win.sdf[i] < 0) side = -1;
    else                     side = 0;

    switch (win.code[i]) {
    case 'N':
      msSpW.netSideband().put(i,side);
      msSpW.freqGroup().put(i,1);
      msSpW.freqGroupName().put(i,"MULTI-CHANNEL-DATA");
      break;
    case 'W':
      msSpW.netSideband().put(i,side);
      msSpW.freqGroup().put(i,3);
      msSpW.freqGroupName().put(i,"SIDE-BAND-AVERAGE");
      break;
    case 'S':
      msSpW.netSideband().put(i,side);
      msSpW.freqGroup().put(i,2);
      msSpW.freqGroupName().put(i,"MULTI-CHANNEL-AVG");
      break;
    default:
      throw(AipsError("Bad code for a spectral window"));
      break;
    }
  }
}


void CarmaFiller::fillFieldTable()
{
  if (DEBUG(1)) cout << "CarmaFiller::fillFieldTable" << endl;

  msc_p->setDirectionRef(epochRef_p);

  MSFieldColumns& msField(msc_p->field());

  Vector<Double> radec(2), pm(2);
  Vector<MDirection> radecMeas(1);
  Int fld;
  Double cosdec;

  pm = 0;                       // Proper motion is zero

  if (nfield == 0) {            // if no pointings found, say there is 1
      WARN ("no dra/ddec pointings found; creating one");
    nfield = npoint = 1;
    dra[0] = ddec[0] = 0.0;
  }

  for (fld = 0; fld < nfield; fld++) {
    int sid = sid_p[fld];

    ms_p.field().addRow();

    if (DEBUG(1))
      cout << "FLD: " << fld << " " << sid << " " << source_p[field[fld]] << endl;

    if (sid > 0) {
      msField.sourceId().put(fld,sid-1);
      msField.name().put(fld,source_p[field[fld]]);        // this is the source name
    } else {
      // a special test where the central source gets _C appended to the source name
      msField.sourceId().put(fld,-sid-1);
      msField.name().put(fld,source_p[field[fld]]);        // or keep them all same name
    }

    msField.code().put(fld,purpose_p[field[fld]]);

    msField.numPoly().put(fld,0);

    cosdec = cos(dec[fld]);
    radec(0) = ra[fld]  + dra[fld]/cosdec;           // RA, in radians
    radec(1) = dec[fld] + ddec[fld];                 // DEC, in radians

    radecMeas(0).set(MVDirection(radec(0), radec(1)), MDirection::Ref(epochRef_p));

    msField.delayDirMeasCol().put(fld,radecMeas);
    msField.phaseDirMeasCol().put(fld,radecMeas);
    msField.referenceDirMeasCol().put(fld,radecMeas);

    // Need to convert epoch in years to MJD time
    if (nearAbs(epoch_p,2000.0,0.01)) {
      msField.time().put(fld, MeasData::MJD2000*C::day);
      // assume UTC epoch
    } else if (nearAbs(epoch_p,1950.0,0.01)) {
      msField.time().put(fld, MeasData::MJDB1950*C::day);
    } else {
      cerr << " Cannot handle epoch "<< epoch_p <<endl;
    }
  }
}


void CarmaFiller::fillSourceTable()
{
  if (DEBUG(1)) cout << "CarmaFiller::fillSourceTable" << endl;
  Int n = win.nspect;
  Int ns = 0;
  Int skip;

  MSSourceColumns& msSource(msc_p->source());

  Vector<Double> radec(2);

  for (uInt src=0; src < source_p.nelements(); src++) {

    skip = 0;                             // check not to duplicate source names
    for (uInt i=0; i<src; i++) {
      if (source_p[src] == source_p[i]) {
	skip=1;
	break;
      }
    }
    if (skip) break;

    ns++;
    ms_p.source().addRow();

    radec(0) = ras_p[src];
    radec(1) = decs_p[src];

    msSource.sourceId().put(src,src);
    msSource.name().put(src,source_p[src]);
    msSource.spectralWindowId().put(src,0);     // FIX it due to a bug in MS2 code (6feb2001)
    msSource.direction().put(src,radec);
    if (n > 0) {
      Int m=n;
      Vector<Double> restFreq(m);
      for (Int i=0; i<m; i++)
	restFreq(i) = win.restfreq[i] * 1e9;    // convert from GHz to Hz

      msSource.numLines().put(src,win.nspect);
      msSource.restFrequency().put(src,restFreq);
    }
    msSource.time().put(src,0.0);               // valid for all times
    msSource.interval().put(src,0);             // valid forever
  }
}


void CarmaFiller::fillFeedTable()
{
  if (DEBUG(1)) cout << "CarmaFiller::fillFeedTable" << endl;

  MSFeedColumns msfc(ms_p.feed());

  // find out the POLARIZATION_TYPE
  // In the fits files we handle there can be only a single, uniform type
  // of polarization so the following should work.
  MSPolarizationColumns& msPolC(msc_p->polarization());

  Int numCorr=msPolC.numCorr()(0);
  Vector<String> rec_type(2); rec_type="";
  if (corrType_p(0)>=Stokes::RR && corrType_p(numCorr-1)<=Stokes::LL) {
      rec_type(0)="R"; rec_type(1)="L";
  }
  if (corrType_p(0)>=Stokes::XX && corrType_p(numCorr-1)<=Stokes::YY) {
      rec_type(0)="X"; rec_type(1)="Y";
  }

  Matrix<Complex> polResponse(2,2);
  polResponse=0.; polResponse(0,0)=polResponse(1,1)=1.;
  Matrix<Double> offset(2,2); offset=0.;
  Vector<Double> position(3); position=0.;
  Vector<Double> ra(2);
  ra = 0.0;

  // fill the feed table
  // will only do UP TO the largest antenna referenced in the dataset
  Int row=-1;
  if (DEBUG(3)) cout << "DEBUG1 :: " << nAnt_p.nelements() << endl;
  for (Int arr=0; arr< (Int)nAnt_p.nelements(); arr++) {
    if (DEBUG(3)) cout << "DEBUG2 :: " << nAnt_p[arr] << endl;
    for (Int ant=0; ant<nAnt_p[arr]; ant++) {
      ms_p.feed().addRow(); row++;

      msfc.antennaId().put(row,ant);
      msfc.beamId().put(row,-1);
      msfc.feedId().put(row,0);
      msfc.interval().put(row,DBL_MAX);

      // msfc.phasedFeedId().put(row,-1);    // now optional
      msfc.spectralWindowId().put(row,-1);
      msfc.time().put(row,0.);
      msfc.numReceptors().put(row,2);
      msfc.beamOffset().put(row,offset);
      msfc.polarizationType().put(row,rec_type);
      msfc.polResponse().put(row,polResponse);
      msfc.position().put(row,position);
      // fix these when incremental array building is ok.
      // although for CARMA this would never change ....
      msfc.receptorAngle().put(row,ra);
      // msfc.receptorAngle().put(row,receptorAngle_p[arr](Slice(2*ant,2)));
    }
  }
}


void CarmaFiller::fixEpochReferences() {

  if (DEBUG(1)) cout << "CarmaFiller::fixEpochReferences" << endl;

  if (timsys_p=="IAT") timsys_p="TAI";
  if (timsys_p=="UTC" || timsys_p=="TAI") {
    String key("MEASURE_REFERENCE");
    MSColumns msc(ms_p);
    msc.time().rwKeywordSet().define(key,timsys_p);
    msc.feed().time().rwKeywordSet().define(key,timsys_p);
    msc.field().time().rwKeywordSet().define(key,timsys_p);
    // Fits obslog time is probably local time instead of TAI or UTC
    //PJT msc.obsLog().time().rwKeywordSet().define(key,timsys_p);
  } else {
    if (timsys_p!="")
      cerr << "Unhandled time reference frame: "<<timsys_p<<endl;
  }
}


void CarmaFiller::Tracking(int record)
{
  if (DEBUG(3)) cout << "CarmaFiller::Tracking" << endl;

  char vdata[10];
  int i, j, k;

  if (record < 0) {                 // first time around: set variables to track
    uvtrack_c(uv_handle_p,"nschan","u");   // narrow lines
    uvtrack_c(uv_handle_p,"nspect","u");   // window averages
    uvtrack_c(uv_handle_p,"ischan","u");
    uvtrack_c(uv_handle_p,"sdf","u");
    uvtrack_c(uv_handle_p,"sfreq","u");    // changes a lot (doppler)

    uvtrack_c(uv_handle_p,"restfreq","u"); // never really changes....
    uvtrack_c(uv_handle_p,"freq","u");     // never really changes....


    uvtrack_c(uv_handle_p,"nwide","u");
    uvtrack_c(uv_handle_p,"wfreq","u");
    uvtrack_c(uv_handle_p,"wwidth","u");

    uvtrack_c(uv_handle_p,"antpos","u");   // array's
    uvtrack_c(uv_handle_p,"dra","u");      // fields
    uvtrack_c(uv_handle_p,"ddec","u");     // fields

    uvtrack_c(uv_handle_p,"ra","u");       // source position
    uvtrack_c(uv_handle_p,"dec","u");      // source position

    uvtrack_c(uv_handle_p,"inttime","u");

    // weather:
    // uvtrack_c(uv_handle_p,"airtemp","u");
    // uvtrack_c(uv_handle_p,"dewpoint","u");
    // uvtrack_c(uv_handle_p,"relhumid","u");
    // uvtrack_c(uv_handle_p,"winddir","u");
    // uvtrack_c(uv_handle_p,"windmph","u");

    return;
  }

  // here is all the special tracking code...

  if (uv_hasvar ("inttime")) {
    uvgetvr_c(uv_handle_p,H_REAL,"inttime",(char *)&inttime_p,1);
  }

  if (uv_hasvar ("antpos") && record) {
      nants_p = uv_getint ("nants");
    uvgetvr_c(uv_handle_p,H_DBLE,"antpos",(char *)antpos,3*nants_p);
    if (DEBUG(2)) {
      cout << "Found " << nants_p << " antennas for array "
	   << nArray_p << endl;
      for (int i=0; i<nants_p; i++) {
        cout << antpos[i] << " " <<
                antpos[i+nants_p] << " " <<
                antpos[i+nants_p*2] << endl;
      }
    }
  }

  if (win.nspect > 0) {
    if (uv_hasvar ("systemp")) {
      uvgetvr_c(uv_handle_p,H_REAL,"systemp",(char *)systemp,nants_p*win.nspect);
      if (DEBUG(3)) {
	cout << "Found systemps (new scan)" ;
	for (Int i=0; i<nants_p; i++)  cout << systemp[i] << " ";
	cout << endl;
      }
    }
  } else {
    if (uv_hasvar ("wsystemp")) {
      uvgetvr_c(uv_handle_p,H_REAL,"wsystemp",(char *)systemp,nants_p);
      if (DEBUG(3)) {
	cout << "Found wsystemps (new scan)" ;
	for (Int i=0; i<nants_p; i++)  cout << systemp[i] << " ";
	cout << endl;
      }
    }
  }

  // SOURCE and DRA/DDEC are mixed together they define a row in the FIELD table
  int source_updated = uv_hasvar ("source");

  if (source_updated) {
    object_p = uv_getstr ("source");


    // bug: as is, source_p will get repeated values, trim it later

    source_p.resize(source_p.nelements()+1, True);     // need to copy the old values
    source_p[source_p.nelements()-1] = object_p;

    ras_p.resize(ras_p.nelements()+1, True);
    decs_p.resize(decs_p.nelements()+1, True);
    ras_p[ras_p.nelements()-1] = 0.0;                  // if no source at (0,0) offset
    decs_p[decs_p.nelements()-1] = 0.0;                // these would never be initialized


    vdata[0] = 'S'; vdata[1] = '\0';
    purpose_p.resize(purpose_p.nelements()+1, True);   // need to copy the old values
    purpose_p[purpose_p.nelements()-1] = vdata;

  }

  if (source_updated || uv_hasvar ("dra") || uv_hasvar ("ddec")) {
    npoint++;
    uvgetvr_c(uv_handle_p,H_DBLE,"ra", (char *)&ra_p, 1);
    uvgetvr_c(uv_handle_p,H_DBLE,"dec",(char *)&dec_p,1);
    dra_p = ddec_p = 0.;
    object_p = uv_getstr ("source");

    for (i = 0, j = -1;  i < (int) source_p.nelements (); i++) {
	if (source_p[i] == object_p) {
	    j = i;
	    break;
	}
    }
    // j should always be >= 0 now, and is the source index

    for (i=0, k=-1; i<nfield; i++) { // check if we had this pointing/source before
      if (dra[i] == dra_p && ddec[i] == ddec_p && field[i] == j) {
	k = i;
	break;
      }
    }
    // k could be -1, when a new field/source is found

    if (DEBUG(1)) {
      cout << "POINTING: " << npoint
	   << " source: " << object_p << " [" << j << "," << k << "] "
	   << " dra/ddec: "   << dra_p << " " << ddec_p << endl;
    }

    if (k<0) {                             // we have a new source/field combination
      ifield = nfield;
      nfield++;
      if (DEBUG(2)) cout << "Adding new field " << ifield
			 << " for " << object_p << source_p[j]
			 << " at "
			 << dra_p *206264.8062 << " "
			 << ddec_p*206264.8062 << " arcsec." << endl;

      if (nfield >= MAXFIELD) {
	cout << "Cannot handle more than " << MAXFIELD << " fields." << endl;
	exit(1);
      }
      ra[ifield]  = ra_p;
      dec[ifield] = dec_p;
      dra[ifield]  = dra_p;
      ddec[ifield] = ddec_p;
      field[ifield] = j;
      sid_p[ifield] = j + 1;
      if (dra_p == 0.0 && ddec_p==0.0) {   // store ra/dec for SOURCE table as well
	ras_p[j]  = ra_p;
	decs_p[j] = dec_p;
	sid_p[ifield] = -sid_p[ifield];    // make central one -index for later NAME change
      }
    } else {
      ifield = k;
    }
  }
}

//
//  this is also a nasty routine. It makes assumptions on
//  a relationship between narrow and window averages
//  which normally exists for CARMA telescope data, but which
//  in principle can be modified by uvcat/uvaver and possibly
//  break this routine...
//  (there has been some talk at the site to write subsets of
//   the full data, which could break this routine)

void CarmaFiller::init_window()
{
  if (DEBUG(1)) cout << "CarmaFiller::init_window" << endl;

  int i, idx, nchan, nspect, nwide;

  if (uv_hasvar ("nchan")) {
    uvrdvr_c(uv_handle_p,H_INT,"nchan",(char *)&nchan, NULL, 1);
  } else {
    nchan = 0;
    if (DEBUG(1)) cout << "nchan = 0" << endl;
  }

  if (uv_hasvar ("nspect")) {
    uvrdvr_c(uv_handle_p,H_INT,"nspect",(char *)&nspect, NULL, 1);
    win.nspect = nspect;
  } else
    win.nspect = nspect = 0;

  if (uv_hasvar ("nwide")) {
    uvrdvr_c(uv_handle_p,H_INT,"nwide",(char *)&nwide, NULL, 1);
    win.nwide = nwide;
  } else
    win.nwide = nwide = 0;

  if (nspect > 0 && nspect <= MAXWIN) {
    if (uv_hasvar ("ischan"))
      uvgetvr_c(uv_handle_p,H_INT,"ischan",(char *)win.ischan, nspect);
    else if (nspect==1)
      win.ischan[0] = 1;
    else
      throw(AipsError("missing ischan"));

    if (uv_hasvar ("nschan"))
      uvgetvr_c(uv_handle_p,H_INT,"nschan",(char *)win.nschan, nspect);
    else if (nspect==1)
      win.nschan[0] = nchan_p;
    else
      throw(AipsError("missing nschan"));

    if (uv_hasvar ("restfreq"))
      uvgetvr_c(uv_handle_p,H_DBLE,"restfreq",(char *)win.restfreq, nspect);
    else
      throw(AipsError("missing restfreq"));

    if (uv_hasvar ("sdf"))
      uvgetvr_c(uv_handle_p,H_DBLE,"sdf",(char *)win.sdf, nspect);
    else if (nspect>1)
      throw(AipsError("missing sdf"));

    if (uv_hasvar ("sfreq"))
      uvgetvr_c(uv_handle_p,H_DBLE,"sfreq",(char *)win.sfreq, nspect);
    else
      throw(AipsError("missing sfreq"));
  }

  if (nwide > 0 && nwide <= MAXWIDE) {
    if (uv_hasvar ("wfreq"))
      uvgetvr_c(uv_handle_p,H_REAL,"wfreq",(char *)win.wfreq, nwide);
    if (uv_hasvar ("wwidth"))
      uvgetvr_c(uv_handle_p,H_REAL,"wwidth",(char *)win.wwidth, nwide);
  }

  for (i=0; i<nspect; i++) {
    win.code[i] = 'N';
    win.keep[i] = 1;
  }

  idx = (nspect > 0 ? nspect : 0);           // idx points into the combined win.xxx[] elements
  for (i=0; i<nwide; i++) {
    Int side = (win.sdf[i] < 0 ? -1 : 1);
    win.code[idx]     = 'S';
    win.keep[idx]     = 1;
    win.ischan[idx]   = nchan + i + 1;
    win.nschan[idx]   = 1;
    win.sfreq[idx]    = win.wfreq[i];
    win.sdf[idx]      = side * win.wwidth[i];
    win.restfreq[idx] = -1.0;  // no meaning
    idx++;
  }

  if (DEBUG(1)) {
    cout << "Layout of spectral windows (init_window): nspect=" << nspect
	 << " nwide=" << nwide
	 << "\n";
    cout << "(N=narrow    W=wide,   S=spectral window averages)" << endl;

    for (i=0; i<nspect+nwide; i++)
      cout << win.code[i] << ": " << i+1  << " " << win.keep[i] << " "
	   << win.nschan[i] << " " << win.ischan[i] << " "
	   << win.sfreq[i] <<  " " << win.sdf[i] <<  " " << win.restfreq[i]
	   << "\n";
  }
}


int
main (int argc, char **argv)
{
    try {
	Input inp (1);
	inp.version ("");
	inp.create ("vis",     "",        "Name of CARMA dataset name",         "string");
	inp.create ("ms",      "",        "Name of MeasurementSet",             "string");
	inp.create ("tsys",    "False",   "Fill WEIGHT from Tsys in data?",     "bool");
	inp.create ("snumbase","0",       "Starting SCAN_NUMBER value",         "int");
	inp.create ("polmode", "0",       "(deprecated; ignored)",              "int");
	inp.readArguments (argc, argv);

	String vis (inp.getString ("vis"));
	if (vis == "")
	    throw AipsError ("no input path (vis=) given");
	if (! File (vis).isDirectory ())
	    throw AipsError ("input path (vis=) does not refer to a directory");

	String ms (inp.getString ("ms"));
	if (ms == "")
	    ms = vis.before ('.') + ".ms";

	Bool apply_tsys = inp.getBool ("tsys");
	Int snumbase = inp.getInt ("snumbase");

	// I don't understand what's going on here:
	int debug = -1;
	while (inp.debug (debug + 1))
	    debug++;

	CarmaFiller cf (vis, debug, apply_tsys);

	cf.checkInput ();
	cf.setupMeasurementSet (ms);
	cf.fillObsTables ();
	cf.fillAntennaTable ();
	cf.fillMSMainTable (True, snumbase);
	cf.fillSyscalTable ();
	cf.fillSpectralWindowTable ();
	cf.fillFieldTable ();
	cf.fillSourceTable ();
	cf.fillFeedTable ();
	cf.fixEpochReferences ();
    } catch (AipsError x) {
	cerr << "error: " << x.getMesg () << endl;
	return 1;
    }

    return 0;
}
