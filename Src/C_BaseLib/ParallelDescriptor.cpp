
#include <cstdio>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
#include <sstream>
#include <stack>

#include <Utility.H>
#include <BLProfiler.H>
#include <ParallelDescriptor.H>
#include <MultiFab.H>
#include <Geometry.H>

namespace ParallelDescriptor
{
    //
    // My processor IDs.
    //
    const int myId_undefined  = -11;
    const int myId_notInGroup = -22;
    int m_MyId_all         = myId_undefined;
    int m_MyId_comp        = myId_undefined;
    int m_MyId_sidecar     = myId_undefined;
    //
    // The number of processors.
    //
    const int nProcs_undefined  = -33;
    int m_nProcs_all     = nProcs_undefined;
    int m_nProcs_comp    = nProcs_undefined;
    int m_nProcs_sidecar = nProcs_undefined;
    int nSidecarProcs    = 0;
    //
    // BoxLib's Communicators
    //
    MPI_Comm m_comm_all     = MPI_COMM_NULL;    // for all procs, probably MPI_COMM_WORLD
    MPI_Comm m_comm_comp    = MPI_COMM_NULL;    // for the computation procs
    MPI_Comm m_comm_sidecar = MPI_COMM_NULL;    // for the in-situ performance monitor
    MPI_Comm m_comm_inter   = MPI_COMM_NULL;    // for communicating between comp and sidecar
    //
    // BoxLib's Groups
    //
    MPI_Group m_group_all     = MPI_GROUP_NULL;
    MPI_Group m_group_comp    = MPI_GROUP_NULL;
    MPI_Group m_group_sidecar = MPI_GROUP_NULL;

    int m_MinTag = 1000, m_MaxTag = -1;

    const int ioProcessor = 0;

    namespace util
    {
	//
	// Reduce helper functons.
	//
	void DoAllReduceReal     (Real&      r, MPI_Op op);
	void DoAllReduceLong     (long&      r, MPI_Op op);
	void DoAllReduceInt      (int&       r, MPI_Op op);

	void DoAllReduceReal     (Real*      r, MPI_Op op, int cnt);
	void DoAllReduceLong     (long*      r, MPI_Op op, int cnt);
	void DoAllReduceInt      (int*       r, MPI_Op op, int cnt);

	void DoReduceReal     (Real&      r, MPI_Op op, int cpu);
	void DoReduceLong     (long&      r, MPI_Op op, int cpu);
	void DoReduceInt      (int&       r, MPI_Op op, int cpu);

	void DoReduceReal     (Real*      r, MPI_Op op, int cnt, int cpu);
	void DoReduceLong     (long*      r, MPI_Op op, int cnt, int cpu);
	void DoReduceInt      (int*       r, MPI_Op op, int cnt, int cpu);
    }

    typedef std::list<ParallelDescriptor::PTR_TO_SIGNAL_HANDLER> SH_LIST;
    SH_LIST The_Signal_Handler_List;
}

void
ParallelDescriptor::AddSignalHandler (PTR_TO_SIGNAL_HANDLER fp)
{
  The_Signal_Handler_List.push_back(fp);
}


#ifndef BL_AMRPROF
//
// Definition of non-inline members of CommData.
//
ParallelDescriptor::CommData::CommData ()
{
    for (int i = 0, N = length(); i < N; i++)
        m_data[i] = 0;
}

ParallelDescriptor::CommData::CommData (int        face,
                                        int        fabindex,
                                        int        fromproc,
                                        int        id,
                                        int        ncomp,
                                        int        srccomp,
                                        int        fabarrayid,
                                        const Box& box)
{
    m_data[0] = face;
    m_data[1] = fabindex;
    m_data[2] = fromproc;
    m_data[3] = id;
    m_data[4] = ncomp;
    m_data[5] = srccomp;
    m_data[6] = fabarrayid;

    const IntVect& sm = box.smallEnd();

    D_TERM(m_data[7] = sm[0];,
           m_data[8] = sm[1];,
           m_data[9] = sm[2];);

    const IntVect& bg = box.bigEnd();

    D_TERM(m_data[7+BL_SPACEDIM] = bg[0];,
           m_data[8+BL_SPACEDIM] = bg[1];,
           m_data[9+BL_SPACEDIM] = bg[2];);

    IntVect typ = box.type();

    D_TERM(m_data[7+2*BL_SPACEDIM] = typ[0];,
           m_data[8+2*BL_SPACEDIM] = typ[1];,
           m_data[9+2*BL_SPACEDIM] = typ[2];);
}

ParallelDescriptor::CommData::CommData (const CommData& rhs)
{
    for (int i = 0, N = length(); i < N; i++)
        m_data[i] = rhs.m_data[i];
}

ParallelDescriptor::CommData&
ParallelDescriptor::CommData::operator= (const CommData& rhs)
{
    if (!(this == &rhs))
    {
        for (int i = 0, N = length(); i < N; i++)
            m_data[i] = rhs.m_data[i];
    }
    return *this;
}

bool
ParallelDescriptor::CommData::operator== (const CommData& rhs) const
{
    for (int i = 0, N = length(); i < N; i++)
        if (!(m_data[i] == rhs.m_data[i]))
            return false;

    return true;
}

std::ostream&
operator<< (std::ostream&                       os,
            const ParallelDescriptor::CommData& cd)
{
    os << cd.face()       << ' '
       << cd.fabindex()   << ' '
       << cd.fromproc()   << ' '
       << cd.id()         << ' '
       << cd.nComp()      << ' '
       << cd.srcComp()    << ' '
       << cd.fabarrayid() << ' '
       << cd.box();

    return os;
}

std::ostream&
operator<< (std::ostream&                              os,
            const Array<ParallelDescriptor::CommData>& cd)
{
    for (int i = 0, N = cd.size(); i < N; i++)
        os << cd[i] << '\n';
    return os;
}
#endif

#ifdef BL_USE_MPI

#include <ccse-mpi.H>

namespace
{
    const char*
    the_message_string (const char* file,
                        int         line,
                        const char* call,
                        int         status)
    {
	const int N = 512;
	static char buf[N];
	if ( status )
	{
	    snprintf(buf, N, "BoxLib MPI Error: File %s, line %d, %s: %s",
                     file, line, call, ParallelDescriptor::ErrorString(status));
	}
	else
	{
	    snprintf(buf, N, "BoxLib MPI Error: File %s, line %d, %s",
                     file, line, call);
	}
	return buf;
    }

}

namespace ParallelDescriptor
{
    void
    MPI_Error (const char* file, int line, const char* str, int rc)
    {
	BoxLib::Error(the_message_string(file, line, str, rc));
    }
}

void
ParallelDescriptor::Abort ()
{
#ifdef WIN32
    throw;
#endif
    MPI_Abort(CommunicatorAll(), -1);
}

void
ParallelDescriptor::Abort (int errorcode)
{
#ifdef BL_BGL
    MPI_Abort(CommunicatorAll(), errorcode);
#else
    BoxLib::Abort(ErrorString(errorcode));
#endif
}

const char*
ParallelDescriptor::ErrorString (int errorcode)
{
    BL_ASSERT(errorcode > 0 && errorcode <= MPI_ERR_LASTCODE);

    int len = 0;

    static char msg[MPI_MAX_ERROR_STRING+1];

    MPI_Error_string(errorcode, msg, &len);

    BL_ASSERT(len <= MPI_MAX_ERROR_STRING);

    return msg;
}

void
ParallelDescriptor::Message::wait ()
{
    BL_PROFILE_S("ParallelDescriptor::Message::wait()");

    BL_COMM_PROFILE(BLProfiler::Wait, sizeof(m_type), pid(), tag());
    BL_MPI_REQUIRE( MPI_Wait(&m_req, &m_stat) );
    BL_COMM_PROFILE(BLProfiler::Wait, sizeof(m_type), BLProfiler::AfterCall(), tag());
}

bool
ParallelDescriptor::Message::test ()
{
    int flag;
    BL_COMM_PROFILE(BLProfiler::Test, sizeof(m_type), pid(), tag());
    BL_MPI_REQUIRE( MPI_Test(&m_req, &flag, &m_stat) );
    BL_COMM_PROFILE(BLProfiler::Test, flag, BLProfiler::AfterCall(), tag());
    m_finished = flag != 0;
    return m_finished;
}

int
ParallelDescriptor::Message::tag () const
{
    if ( !m_finished ) BoxLib::Error("Message::tag: Not Finished!");
    return m_stat.MPI_TAG;
}

int
ParallelDescriptor::Message::pid () const
{
    if ( !m_finished ) BoxLib::Error("Message::pid: Not Finished!");
    return m_stat.MPI_SOURCE;
}

size_t
ParallelDescriptor::Message::count () const
{
    if ( m_type == MPI_DATATYPE_NULL ) BoxLib::Error("Message::count: Bad Type!");
    if ( !m_finished ) BoxLib::Error("Message::count: Not Finished!");
    int cnt;
    BL_MPI_REQUIRE( MPI_Get_count(&m_stat, m_type, &cnt) );
    return cnt;
}

void
ParallelDescriptor::StartParallel (int*    argc,
                                   char*** argv,
                                   MPI_Comm mpi_comm)
{
    m_comm_all = mpi_comm;

    int sflag(0);

    BL_MPI_REQUIRE( MPI_Initialized(&sflag) );

    if ( ! sflag) {
	BL_MPI_REQUIRE( MPI_Init(argc, argv) );
    }
    
    BL_MPI_REQUIRE( MPI_Comm_size(CommunicatorAll(), &m_nProcs_all) );
    BL_MPI_REQUIRE( MPI_Comm_rank(CommunicatorAll(), &m_MyId_all) );

    if(m_MyId_all == 0 && nSidecarProcs > 0) {
      std::cout << "**** nSidecarProcs = " << nSidecarProcs << std::endl;
    }
    if(nSidecarProcs >= m_nProcs_all) {
      std::cerr << "**** nSidecarProcs >= m_nProcs_all:  " << nSidecarProcs
                << " >= " << m_nProcs_all << std::endl;
      BoxLib::Abort("Error in StartParallel:  bad nSidecarProcs.");
    }

    MPI_Comm_group(m_comm_all, &m_group_all);

    if(nSidecarProcs > 0) {
      Array<int> sidecarRanksInAll(nSidecarProcs, nProcs_undefined);
      for(int ip(0); ip < nSidecarProcs; ++ip) {
        sidecarRanksInAll[ip] = m_nProcs_all - nSidecarProcs + ip;
      }
      MPI_Group_excl(m_group_all, nSidecarProcs, sidecarRanksInAll.dataPtr(), &m_group_comp);
      MPI_Comm_create(m_comm_all, m_group_comp, &m_comm_comp);

      MPI_Group_incl(m_group_all, nSidecarProcs, sidecarRanksInAll.dataPtr(), &m_group_sidecar);
      MPI_Comm_create(m_comm_all, m_group_sidecar, &m_comm_sidecar);

      MPI_Group_size(m_group_sidecar, &m_nProcs_sidecar);
      MPI_Group_size(m_group_comp, &m_nProcs_comp);
    } else {
      m_comm_comp  = m_comm_all;
      m_group_comp = m_group_all;

      m_nProcs_comp = m_nProcs_all;
      m_nProcs_sidecar = 0;
    }

    // ---- find the maximum value for a tag
    int flag(0), *attrVal;
    BL_MPI_REQUIRE( MPI_Attr_get(m_comm_all, MPI_TAG_UB, &attrVal, &flag) );
    if(flag) {
      m_MaxTag = *attrVal;
      m_MaxTag -= 4;  // so we dont wrap if maxint
      m_MaxTag = std::max(m_MaxTag, 9000);
    } else {
      m_MaxTag = 9000;
    }
    BL_COMM_PROFILE_TAGRANGE(m_MinTag, m_MaxTag);

    if(nSidecarProcs > 0) {
      int tag(m_MaxTag + 1);


      if(m_MyId_all >= m_nProcs_all - nSidecarProcs) {  // ---- sidecar group
        MPI_Group_rank(m_group_sidecar, &m_MyId_sidecar);
        MPI_Intercomm_create(m_comm_sidecar, 0, m_comm_all, 0, tag, &m_comm_inter);
	m_MyId_comp = myId_notInGroup;

      } else {                        // ---- in computation group
        MPI_Group_rank(m_group_comp, &m_MyId_comp);
        MPI_Intercomm_create(m_comm_comp, 0, m_comm_all, m_nProcs_all - nSidecarProcs,
	                     tag, &m_comm_inter);
	m_MyId_sidecar = myId_notInGroup;
      }

    } else {
      m_MyId_comp   = m_MyId_all;
      m_MyId_sidecar   = myId_notInGroup;
    }

    //
    // Wait until all other processes are properly started.
    //
    BL_MPI_REQUIRE( MPI_Barrier(CommunicatorAll()) );

    if(m_MyId_all     == myId_undefined ||
       m_MyId_comp    == myId_undefined ||
       m_MyId_sidecar == myId_undefined)
    {
      std::cerr << "m_MyId_all m_MyId_comp m_MyId_sidecar = " << m_MyId_all << "  "
	          << m_MyId_comp << "  " << m_MyId_sidecar << std::endl;
      BoxLib::Abort("**** Error:  bad MyId in ParallelDescriptor::StartParallel()");
    }
    if(m_nProcs_all     == nProcs_undefined ||
       m_nProcs_comp    == nProcs_undefined ||
       m_nProcs_sidecar == nProcs_undefined ||
       (m_nProcs_comp + m_nProcs_sidecar != m_nProcs_all))
    {
      std::cerr << "m_nProcs_all m_nProcs_comp m_nProcs_sidecar = " << m_nProcs_all << "  "
	          << m_nProcs_comp << "  " << m_nProcs_sidecar << std::endl;
      BoxLib::Abort("**** Error:  bad nProcs in ParallelDescriptor::StartParallel()");
    }
}

void
ParallelDescriptor::EndParallel ()
{
    BL_ASSERT(m_MyId_all != -1);
    BL_ASSERT(m_nProcs_all != -1);

    BL_MPI_REQUIRE( MPI_Finalize() );
}

double
ParallelDescriptor::second ()
{
    return MPI_Wtime();
}

void
ParallelDescriptor::Barrier (const std::string &message)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::Barrier()");
    BL_COMM_PROFILE_BARRIER(message, true);

    BL_MPI_REQUIRE( MPI_Barrier(ParallelDescriptor::Communicator()) );

    BL_COMM_PROFILE_BARRIER(message, false);
}

void
ParallelDescriptor::Barrier (MPI_Comm comm, const std::string &message)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::Barrier(comm)");
    BL_COMM_PROFILE_BARRIER(message, true);

    BL_MPI_REQUIRE( MPI_Barrier(comm) );

    BL_COMM_PROFILE_BARRIER(message, false);
}

void
ParallelDescriptor::Test (MPI_Request& request, int& flag, MPI_Status& status)
{
    BL_PROFILE_S("ParallelDescriptor::Test()");
    BL_COMM_PROFILE(BLProfiler::Test, sizeof(char), status.MPI_SOURCE, status.MPI_TAG);

    BL_MPI_REQUIRE( MPI_Test(&request,&flag,&status) );

    BL_COMM_PROFILE(BLProfiler::Test, flag, BLProfiler::AfterCall(), status.MPI_TAG);
}

void
ParallelDescriptor::IProbe (int src_pid, int tag, int& flag, MPI_Status& status)
{
    BL_PROFILE_S("ParallelDescriptor::Iprobe()");
    BL_COMM_PROFILE(BLProfiler::Iprobe, sizeof(char), src_pid, tag);

    BL_MPI_REQUIRE( MPI_Iprobe(src_pid, tag, ParallelDescriptor::Communicator(),
                               &flag, &status) );

    BL_COMM_PROFILE(BLProfiler::Iprobe, flag, BLProfiler::AfterCall(), status.MPI_TAG);
}

void
ParallelDescriptor::IProbe (int src_pid, int tag, MPI_Comm comm, int& flag, MPI_Status& status)
{
    BL_PROFILE_S("ParallelDescriptor::Iprobe(comm)");
    BL_COMM_PROFILE(BLProfiler::Iprobe, sizeof(char), src_pid, tag);

    BL_MPI_REQUIRE( MPI_Iprobe(src_pid, tag, comm,
                               &flag, &status) );

    BL_COMM_PROFILE(BLProfiler::Iprobe, flag, BLProfiler::AfterCall(), status.MPI_TAG);
}

void
ParallelDescriptor::Comm_dup (MPI_Comm comm, MPI_Comm& newcomm)
{
    BL_PROFILE_S("ParallelDescriptor::Comm_dup()");
    BL_MPI_REQUIRE( MPI_Comm_dup(comm, &newcomm) );
}

void
ParallelDescriptor::util::DoAllReduceReal (Real&  r,
                                           MPI_Op op)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoAllReduceReal()");
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceR, BLProfiler::BeforeCall(), true);

    Real recv;

    BL_MPI_REQUIRE( MPI_Allreduce(&r,
                                  &recv,
                                  1,
                                  Mpi_typemap<Real>::type(),
                                  op,
                                  Communicator()) );
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceR, sizeof(Real), false);
    r = recv;
}

void
ParallelDescriptor::util::DoAllReduceReal (Real*  r,
                                           MPI_Op op,
                                           int    cnt)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoAllReduceReal()");
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceR, BLProfiler::BeforeCall(), true);

    BL_ASSERT(cnt > 0);

    Array<Real> recv(cnt);

    BL_MPI_REQUIRE( MPI_Allreduce(r,
                                  recv.dataPtr(),
                                  cnt,
                                  Mpi_typemap<Real>::type(),
                                  op,
                                  Communicator()) );
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceR, cnt * sizeof(Real), false);
    for (int i = 0; i < cnt; i++)
        r[i] = recv[i];
}

void
ParallelDescriptor::util::DoReduceReal (Real&  r,
                                        MPI_Op op,
                                        int    cpu)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoReduceReal()");
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceR, sizeof(Real), cpu);

    Real recv;

    BL_MPI_REQUIRE( MPI_Reduce(&r,
                               &recv,
                               1,
                               Mpi_typemap<Real>::type(),
                               op,
                               cpu,
                               Communicator()) );
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceR, BLProfiler::AfterCall(), cpu);

    if (ParallelDescriptor::MyProc() == cpu)
        r = recv;
}

void
ParallelDescriptor::util::DoReduceReal (Real*  r,
                                        MPI_Op op,
                                        int    cnt,
                                        int    cpu)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoReduceReal()");
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceR, cnt * sizeof(Real), cpu);

    BL_ASSERT(cnt > 0);

    Array<Real> recv(cnt);

    BL_MPI_REQUIRE( MPI_Reduce(r,
                               recv.dataPtr(),
                               cnt,
                               Mpi_typemap<Real>::type(),
                               op,
                               cpu,
                               Communicator()) );
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceR, BLProfiler::AfterCall(), cpu);

    if (ParallelDescriptor::MyProc() == cpu)
    {
        for (int i = 0; i < cnt; i++)
            r[i] = recv[i];
    }
}

void
ParallelDescriptor::ReduceRealMax (Real& r)
{
    util::DoAllReduceReal(r,MPI_MAX);
}

void
ParallelDescriptor::ReduceRealMin (Real& r)
{
    util::DoAllReduceReal(r,MPI_MIN);
}

void
ParallelDescriptor::ReduceRealSum (Real& r)
{
    util::DoAllReduceReal(r,MPI_SUM);
}

void
ParallelDescriptor::ReduceRealMax (Real* r, int cnt)
{
    util::DoAllReduceReal(r,MPI_MAX,cnt);
}

void
ParallelDescriptor::ReduceRealMin (Real* r, int cnt)
{
    util::DoAllReduceReal(r,MPI_MIN,cnt);
}

void
ParallelDescriptor::ReduceRealSum (Real* r, int cnt)
{
    util::DoAllReduceReal(r,MPI_SUM,cnt);
}

void
ParallelDescriptor::ReduceRealMax (Real& r, int cpu)
{
    util::DoReduceReal(r,MPI_MAX,cpu);
}

void
ParallelDescriptor::ReduceRealMin (Real& r, int cpu)
{
    util::DoReduceReal(r,MPI_MIN,cpu);
}

void
ParallelDescriptor::ReduceRealSum (Real& r, int cpu)
{
    util::DoReduceReal(r,MPI_SUM,cpu);
}

void
ParallelDescriptor::ReduceRealMax (Real* r, int cnt, int cpu)
{
    util::DoReduceReal(r,MPI_MAX,cnt,cpu);
}


void
ParallelDescriptor::ReduceRealMin (Real* r, int cnt, int cpu)
{
    util::DoReduceReal(r,MPI_MIN,cnt,cpu);
}

void
ParallelDescriptor::ReduceRealSum (Real* r, int cnt, int cpu)
{
    util::DoReduceReal(r,MPI_SUM,cnt,cpu);
}

void
ParallelDescriptor::util::DoAllReduceLong (long&  r,
                                           MPI_Op op)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoAllReduceLong()");
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceL, BLProfiler::BeforeCall(), true);

    long recv;

    BL_MPI_REQUIRE( MPI_Allreduce(&r,
                                  &recv,
                                  1,
                                  MPI_LONG,
                                  op,
                                  Communicator()) );
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceL, sizeof(long), false);
    r = recv;
}

void
ParallelDescriptor::util::DoAllReduceLong (long*  r,
                                           MPI_Op op,
                                           int    cnt)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoAllReduceLong()");
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceL, BLProfiler::BeforeCall(), true);

    BL_ASSERT(cnt > 0);

    Array<long> recv(cnt);

    BL_MPI_REQUIRE( MPI_Allreduce(r,
                                  recv.dataPtr(),
                                  cnt,
                                  MPI_LONG,
                                  op,
                                  Communicator()) );
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceL, cnt * sizeof(long), false);
    for (int i = 0; i < cnt; i++)
        r[i] = recv[i];
}

void
ParallelDescriptor::util::DoReduceLong (long&  r,
                                        MPI_Op op,
                                        int    cpu)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoReduceLong()");
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceL, sizeof(long), cpu);

    long recv;

    BL_MPI_REQUIRE( MPI_Reduce(&r,
                               &recv,
                               1,
                               MPI_LONG,
                               op,
                               cpu,
                               Communicator()));
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceL, BLProfiler::AfterCall(), cpu);

    if (ParallelDescriptor::MyProc() == cpu)
        r = recv;
}

void
ParallelDescriptor::util::DoReduceLong (long*  r,
                                        MPI_Op op,
                                        int    cnt,
                                        int    cpu)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoReduceLong()");
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceL, cnt * sizeof(long), cpu);

    BL_ASSERT(cnt > 0);

    Array<long> recv(cnt);

    BL_MPI_REQUIRE( MPI_Reduce(r,
                               recv.dataPtr(),
                               cnt,
                               MPI_LONG,
                               op,
                               cpu,
                               Communicator()));
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceL, BLProfiler::AfterCall(), cpu);

    if (ParallelDescriptor::MyProc() == cpu)
    {
        for (int i = 0; i < cnt; i++)
            r[i] = recv[i];
    }
}

void
ParallelDescriptor::ReduceLongAnd (long& r)
{
    util::DoAllReduceLong(r,MPI_LAND);
}

void
ParallelDescriptor::ReduceLongSum (long& r)
{
    util::DoAllReduceLong(r,MPI_SUM);
}

void
ParallelDescriptor::ReduceLongMax (long& r)
{
    util::DoAllReduceLong(r,MPI_MAX);
}

void
ParallelDescriptor::ReduceLongMin (long& r)
{
    util::DoAllReduceLong(r,MPI_MIN);
}

void
ParallelDescriptor::ReduceLongAnd (long* r, int cnt)
{
    util::DoAllReduceLong(r,MPI_LAND,cnt);
}

void
ParallelDescriptor::ReduceLongSum (long* r, int cnt)
{
    util::DoAllReduceLong(r,MPI_SUM,cnt);
}

void
ParallelDescriptor::ReduceLongMax (long* r, int cnt)
{
    util::DoAllReduceLong(r,MPI_MAX,cnt);
}

void
ParallelDescriptor::ReduceLongMin (long* r, int cnt)
{
    util::DoAllReduceLong(r,MPI_MIN,cnt);
}

void
ParallelDescriptor::ReduceLongAnd (long& r, int cpu)
{
    util::DoReduceLong(r,MPI_LAND,cpu);
}

void
ParallelDescriptor::ReduceLongSum (long& r, int cpu)
{
    util::DoReduceLong(r,MPI_SUM,cpu);
}

void
ParallelDescriptor::ReduceLongMax (long& r, int cpu)
{
    util::DoReduceLong(r,MPI_MAX,cpu);
}

void
ParallelDescriptor::ReduceLongMin (long& r, int cpu)
{
    util::DoReduceLong(r,MPI_MIN,cpu);
}

void
ParallelDescriptor::ReduceLongAnd (long* r, int cnt, int cpu)
{
    util::DoReduceLong(r,MPI_LAND,cnt,cpu);
}

void
ParallelDescriptor::ReduceLongSum (long* r, int cnt, int cpu)
{
    util::DoReduceLong(r,MPI_SUM,cnt,cpu);
}

void
ParallelDescriptor::ReduceLongMax (long* r, int cnt, int cpu)
{
    util::DoReduceLong(r,MPI_MAX,cnt,cpu);
}

void
ParallelDescriptor::ReduceLongMin (long* r, int cnt, int cpu)
{
    util::DoReduceLong(r,MPI_MIN,cnt,cpu);
}

void
ParallelDescriptor::util::DoAllReduceInt (int&   r,
                                          MPI_Op op)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoAllReduceInt()");
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceI, BLProfiler::BeforeCall(), true);

    int recv;

    BL_MPI_REQUIRE( MPI_Allreduce(&r,
                                  &recv,
                                  1,
                                  MPI_INT,
                                  op,
                                  Communicator()));
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceI, sizeof(int), false);
    r = recv;
}

void
ParallelDescriptor::util::DoAllReduceInt (int*   r,
                                          MPI_Op op,
                                          int    cnt)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoAllReduceInt()");
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceI, BLProfiler::BeforeCall(), true);

    BL_ASSERT(cnt > 0);

    Array<int> recv(cnt);

    BL_MPI_REQUIRE( MPI_Allreduce(r,
                                  recv.dataPtr(),
                                  cnt,
                                  MPI_INT,
                                  op,
                                  Communicator()));
    BL_COMM_PROFILE_ALLREDUCE(BLProfiler::AllReduceI, cnt * sizeof(int), false);
    for (int i = 0; i < cnt; i++)
        r[i] = recv[i];
}

void
ParallelDescriptor::util::DoReduceInt (int&   r,
                                       MPI_Op op,
                                       int    cpu)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoReduceInt()");
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceI, sizeof(int), cpu);

    int recv;

    BL_MPI_REQUIRE( MPI_Reduce(&r,
                               &recv,
                               1,
                               MPI_INT,
                               op,
                               cpu,
                               Communicator()));
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceI, BLProfiler::AfterCall(), cpu);

    if (ParallelDescriptor::MyProc() == cpu)
        r = recv;
}

void
ParallelDescriptor::util::DoReduceInt (int*   r,
                                       MPI_Op op,
                                       int    cnt,
                                       int    cpu)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::util::DoReduceInt()");
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceI, cnt * sizeof(int), cpu);

    BL_ASSERT(cnt > 0);

    Array<int> recv(cnt);

    BL_MPI_REQUIRE( MPI_Reduce(r,
                               recv.dataPtr(),
                               cnt,
                               MPI_INT,
                               op,
                               cpu,
                               Communicator()));
    BL_COMM_PROFILE_REDUCE(BLProfiler::ReduceI, BLProfiler::AfterCall(), cpu);

    if (ParallelDescriptor::MyProc() == cpu)
    {
        for (int i = 0; i < cnt; i++)
            r[i] = recv[i];
    }
}

void
ParallelDescriptor::ReduceIntSum (int& r)
{
    util::DoAllReduceInt(r,MPI_SUM);
}

void
ParallelDescriptor::ReduceIntMax (int& r)
{
    util::DoAllReduceInt(r,MPI_MAX);
}

void
ParallelDescriptor::ReduceIntMin (int& r)
{
    util::DoAllReduceInt(r,MPI_MIN);
}

void
ParallelDescriptor::ReduceIntSum (int* r, int cnt)
{
    util::DoAllReduceInt(r,MPI_SUM,cnt);
}

void
ParallelDescriptor::ReduceIntMax (int* r, int cnt)
{
    util::DoAllReduceInt(r,MPI_MAX,cnt);
}

void
ParallelDescriptor::ReduceIntMin (int* r, int cnt)
{
    util::DoAllReduceInt(r,MPI_MIN,cnt);
}

void
ParallelDescriptor::ReduceIntSum (int& r, int cpu)
{
    util::DoReduceInt(r,MPI_SUM,cpu);
}

void
ParallelDescriptor::ReduceIntMax (int& r, int cpu)
{
    util::DoReduceInt(r,MPI_MAX,cpu);
}

void
ParallelDescriptor::ReduceIntMin (int& r, int cpu)
{
    util::DoReduceInt(r,MPI_MIN,cpu);
}

void
ParallelDescriptor::ReduceIntSum (int* r, int cnt, int cpu)
{
    util::DoReduceInt(r,MPI_SUM,cnt,cpu);
}

void
ParallelDescriptor::ReduceIntMax (int* r, int cnt, int cpu)
{
    util::DoReduceInt(r,MPI_MAX,cnt,cpu);
}

void
ParallelDescriptor::ReduceIntMin (int* r, int cnt, int cpu)
{
    util::DoReduceInt(r,MPI_MIN,cnt,cpu);
}

void
ParallelDescriptor::ReduceBoolAnd (bool& r)
{
    int src = r; // src is either 0 or 1.

    util::DoAllReduceInt(src,MPI_SUM);

    r = (src == ParallelDescriptor::NProcs()) ? true : false;
}

void
ParallelDescriptor::ReduceBoolOr (bool& r)
{
    int src = r; // src is either 0 or 1.

    util::DoAllReduceInt(src,MPI_SUM);

    r = (src == 0) ? false : true;
}

void
ParallelDescriptor::ReduceBoolAnd (bool& r, int cpu)
{
    int src = r; // src is either 0 or 1.

    util::DoReduceInt(src,MPI_SUM,cpu);

    if (ParallelDescriptor::MyProc() == cpu)
        r = (src == ParallelDescriptor::NProcs()) ? true : false;
}

void
ParallelDescriptor::ReduceBoolOr (bool& r, int cpu)
{
    int src = r; // src is either 0 or 1.

    util::DoReduceInt(src,MPI_SUM,cpu);

    if (ParallelDescriptor::MyProc() == cpu)
        r = (src == 0) ? false : true;
}

void
ParallelDescriptor::Gather (Real* sendbuf,
                            int   nsend,
                            Real* recvbuf,
                            int   root)
{
    BL_PROFILE_S("ParallelDescriptor::Gather()");
    BL_COMM_PROFILE(BLProfiler::GatherRiRi, BLProfiler::BeforeCall(), root, BLProfiler::NoTag());

    BL_ASSERT(root >= 0);
    BL_ASSERT(nsend > 0);
    BL_ASSERT(!(sendbuf == 0));
    BL_ASSERT(!(recvbuf == 0));

    MPI_Datatype typ = Mpi_typemap<Real>::type();

    BL_MPI_REQUIRE( MPI_Gather(sendbuf,
                               nsend,
                               typ,
                               recvbuf,
                               nsend,
                               typ,
                               root,
                               Communicator()));
    BL_COMM_PROFILE(BLProfiler::GatherRiRi, nsend * sizeof(Real), root, BLProfiler::NoTag());
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<char>::type ()
{
    return  MPI_CHAR;
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<short>::type ()
{
    return  MPI_SHORT;
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<int>::type ()
{
    return  MPI_INT;
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<long>::type ()
{
    return  MPI_LONG;
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<unsigned char>::type ()
{
    return  MPI_UNSIGNED_CHAR;
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<unsigned short>::type ()
{
    return  MPI_UNSIGNED_SHORT;
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<unsigned int>::type ()
{
    return  MPI_UNSIGNED;
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<unsigned long>::type ()
{
    return  MPI_UNSIGNED_LONG;
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<float>::type ()
{
    return  MPI_FLOAT;
}

template <>
MPI_Datatype
ParallelDescriptor::Mpi_typemap<double>::type ()
{
    return  MPI_DOUBLE;
}

void
ParallelDescriptor::Waitsome (Array<MPI_Request>& reqs,
                              int&                completed,
                              Array<int>&         indx,
                              Array<MPI_Status>&  status)
{
    BL_PROFILE_S("ParallelDescriptor::Waitsome()");
#ifdef JEFF_TEST
    std::vector<MPI_Request> rq;
    for (int i = 0; i < reqs.size(); i++)
        if (reqs[i] != MPI_REQUEST_NULL)
            rq.push_back(reqs[i]);
    std::vector<MPI_Status> rst(rq.size());

    BL_COMM_PROFILE_WAITSOME(BLProfiler::Waitall, reqs, completed, indx, status, true);
    BL_MPI_REQUIRE( MPI_Waitall(rq.size(), &rq[0], &rst[0]) );
    BL_COMM_PROFILE_WAITSOME(BLProfiler::Waitall, reqs, completed, indx, status, false);
    completed = rq.size();
    int c = 0;
    for ( int i = 0; i < reqs.size(); ++i )
        if (reqs[i] != MPI_REQUEST_NULL)
    {
	reqs[i] = rq[c];
	status[i] = rst[c];
	indx[c] = i;
	c++;
    }
#else
    BL_COMM_PROFILE_WAITSOME(BLProfiler::Waitsome, reqs, completed, indx, status, true);
    BL_MPI_REQUIRE( MPI_Waitsome(reqs.size(),
                                 reqs.dataPtr(),
                                 &completed,
                                 indx.dataPtr(),
                                 status.dataPtr()));
    BL_COMM_PROFILE_WAITSOME(BLProfiler::Waitsome, reqs, completed, indx, status, false);
#endif
}

void
ParallelDescriptor::Bcast(void *buf,
                          int count,
                          MPI_Datatype datatype,
                          int root,
                          MPI_Comm comm)
{
#ifdef BL_LAZY
    Lazy::EvalReduction();
#endif

    BL_PROFILE_S("ParallelDescriptor::Bcast(viMiM)");
    BL_COMM_PROFILE(BLProfiler::BCastTsi, BLProfiler::BeforeCall(), root, BLProfiler::NoTag());

    BL_MPI_REQUIRE( MPI_Bcast(buf,
                              count,
                              datatype,
                              root,
                              comm) );
    int tsize(0);
    BL_MPI_REQUIRE( MPI_Type_size(datatype, &tsize) );
    BL_COMM_PROFILE(BLProfiler::BCastTsi, count * tsize, root, BLProfiler::NoTag());
}


#else /*!BL_USE_MPI*/

void
ParallelDescriptor::StartParallel (int*    argc,
                                   char*** argv,
                                   MPI_Comm)
{
    m_nProcs_all     = 1;
    m_nProcs_comp    = 1;
    m_nProcs_sidecar = 0;
    nSidecarProcs    = 0;

    m_MyId_all     = 0;
    m_MyId_comp    = 0;
    m_MyId_sidecar = myId_notInGroup;

    m_comm_all     = 0;
    m_comm_comp    = 0;
    m_comm_sidecar = 0;
    m_comm_inter   = 0;

    m_MaxTag    = 9000;
}

void
ParallelDescriptor::Gather (Real* sendbuf,
			    int   nsend,
			    Real* recvbuf,
			    int   root)
{
    BL_ASSERT(root == 0);
    BL_ASSERT(nsend > 0);
    BL_ASSERT(!(sendbuf == 0));
    BL_ASSERT(!(recvbuf == 0));

    for (int i = 0; i < nsend; ++i)
        recvbuf[i] = sendbuf[i];
}

void
ParallelDescriptor::Message::wait ()
{}

bool
ParallelDescriptor::Message::test ()
{
    return m_finished;
}

void ParallelDescriptor::EndParallel () {}

void ParallelDescriptor::Abort ()
{ 
#ifdef WIN32
    throw;
#else
    std::abort(); 
#endif
}
void ParallelDescriptor::Abort (int)
{ 
#ifdef WIN32
    throw;
#else
    std::abort(); 
#endif
}

const char* ParallelDescriptor::ErrorString (int) { return ""; }

void ParallelDescriptor::Barrier (const std::string &message) {}
void ParallelDescriptor::Barrier (MPI_Comm, const std::string &message) {}

void ParallelDescriptor::Test (MPI_Request&, int&, MPI_Status&) {}
void ParallelDescriptor::IProbe (int, int, int&, MPI_Status&) {}
void ParallelDescriptor::IProbe (int, int, MPI_Comm, int&, MPI_Status&) {}

void ParallelDescriptor::Comm_dup (MPI_Comm, MPI_Comm&) {}

void ParallelDescriptor::ReduceRealMax (Real&) {}
void ParallelDescriptor::ReduceRealMin (Real&) {}
void ParallelDescriptor::ReduceRealSum (Real&) {}

void ParallelDescriptor::ReduceRealMax (Real&,int) {}
void ParallelDescriptor::ReduceRealMin (Real&,int) {}
void ParallelDescriptor::ReduceRealSum (Real&,int) {}

void ParallelDescriptor::ReduceRealMax (Real*,int) {}
void ParallelDescriptor::ReduceRealMin (Real*,int) {}
void ParallelDescriptor::ReduceRealSum (Real*,int) {}

void ParallelDescriptor::ReduceRealMax (Real*,int,int) {}
void ParallelDescriptor::ReduceRealMin (Real*,int,int) {}
void ParallelDescriptor::ReduceRealSum (Real*,int,int) {}

void ParallelDescriptor::ReduceLongAnd (long&) {}
void ParallelDescriptor::ReduceLongSum (long&) {}
void ParallelDescriptor::ReduceLongMax (long&) {}
void ParallelDescriptor::ReduceLongMin (long&) {}

void ParallelDescriptor::ReduceLongAnd (long&,int) {}
void ParallelDescriptor::ReduceLongSum (long&,int) {}
void ParallelDescriptor::ReduceLongMax (long&,int) {}
void ParallelDescriptor::ReduceLongMin (long&,int) {}

void ParallelDescriptor::ReduceLongAnd (long*,int) {}
void ParallelDescriptor::ReduceLongSum (long*,int) {}
void ParallelDescriptor::ReduceLongMax (long*,int) {}
void ParallelDescriptor::ReduceLongMin (long*,int) {}

void ParallelDescriptor::ReduceLongAnd (long*,int,int) {}
void ParallelDescriptor::ReduceLongSum (long*,int,int) {}
void ParallelDescriptor::ReduceLongMax (long*,int,int) {}
void ParallelDescriptor::ReduceLongMin (long*,int,int) {}

void ParallelDescriptor::ReduceIntSum (int&) {}
void ParallelDescriptor::ReduceIntMax (int&) {}
void ParallelDescriptor::ReduceIntMin (int&) {}

void ParallelDescriptor::ReduceIntSum (int&,int) {}
void ParallelDescriptor::ReduceIntMax (int&,int) {}
void ParallelDescriptor::ReduceIntMin (int&,int) {}

void ParallelDescriptor::ReduceIntSum (int*,int) {}
void ParallelDescriptor::ReduceIntMax (int*,int) {}
void ParallelDescriptor::ReduceIntMin (int*,int) {}

void ParallelDescriptor::ReduceIntSum (int*,int,int) {}
void ParallelDescriptor::ReduceIntMax (int*,int,int) {}
void ParallelDescriptor::ReduceIntMin (int*,int,int) {}

void ParallelDescriptor::ReduceBoolAnd (bool&) {}
void ParallelDescriptor::ReduceBoolOr  (bool&) {}

void ParallelDescriptor::ReduceBoolAnd (bool&,int) {}
void ParallelDescriptor::ReduceBoolOr  (bool&,int) {}

void ParallelDescriptor::Bcast(void *, int, MPI_Datatype, int, MPI_Comm) {}

//
// Here so we don't need to include <Utility.H> in <ParallelDescriptor.H>.
//
double
ParallelDescriptor::second ()
{
    return BoxLib::wsecond();
}

void
ParallelDescriptor::Waitsome (Array<MPI_Request>& reqs,
                              int&                completed,
                              Array<int>&         indx,
                              Array<MPI_Status>&  status)
{}

#endif
//
// This function is the same whether or not we're using MPI.
//
int
ParallelDescriptor::SeqNum ()
{
    static int seqno = m_MinTag;

    int result = seqno++;

    if (seqno > m_MaxTag) {
      seqno = m_MinTag;
      BL_COMM_PROFILE_TAGWRAP();
    }

    return result;
}


#include <BLFort.H>

BL_FORT_PROC_DECL(BL_PD_BARRIER,bl_pd_barrier)()
{
    ParallelDescriptor::Barrier();
}

BL_FORT_PROC_DECL(BL_PD_COMMUNICATOR,bl_pd_communicator)(void* vcomm)
{
    MPI_Comm* comm = reinterpret_cast<MPI_Comm*>(vcomm);

    *comm = ParallelDescriptor::Communicator();
}

BL_FORT_PROC_DECL(BL_PD_MYPROC,bl_pd_myproc)(int* myproc)
{
    *myproc = ParallelDescriptor::MyProc();
}

BL_FORT_PROC_DECL(BL_PD_NPROCS,bl_pd_nprocs)(int* nprocs)
{
    *nprocs = ParallelDescriptor::NProcs();
}

BL_FORT_PROC_DECL(BL_PD_IOPROC,bl_pd_ioproc)(int* ioproc)
{
    *ioproc = ParallelDescriptor::IOProcessorNumber();
}

BL_FORT_PROC_DECL(BL_PD_IS_IOPROC,bl_pd_is_ioproc)(int* ioproc)
{
    *ioproc = ParallelDescriptor::IOProcessor()?1:0;
}

BL_FORT_PROC_DECL(BL_PD_SECOND,bl_pd_second)(double* r)
{
    *r = ParallelDescriptor::second();
}

#ifdef BL_USE_FLOAT
BL_FORT_PROC_DECL(BL_PD_REDUCE_REAL_MAX_TO_IOPROC,bl_pd_reduce_real_max_to_ioproc)(float* r)
{
    ParallelDescriptor::ReduceRealMax(*r,ParallelDescriptor::IOProcessorNumber());
}

BL_FORT_PROC_DECL(BL_PD_REDUCE_REAL_SUM_TO_IOPROC,bl_pd_reduce_real_sum_to_ioproc)(float* r)
{
    ParallelDescriptor::ReduceRealSum(*r,ParallelDescriptor::IOProcessorNumber());
}
#else
BL_FORT_PROC_DECL(BL_PD_REDUCE_REAL_MAX_TO_IOPROC,bl_pd_reduce_real_max_to_ioproc)(double* r)
{
    ParallelDescriptor::ReduceRealMax(*r,ParallelDescriptor::IOProcessorNumber());
}

BL_FORT_PROC_DECL(BL_PD_REDUCE_REAL_SUM_TO_IOPROC,bl_pd_reduce_real_sum_to_ioproc)(double* r)
{
    ParallelDescriptor::ReduceRealSum(*r,ParallelDescriptor::IOProcessorNumber());
}
#endif

BL_FORT_PROC_DECL(BL_PD_ABORT,bl_pd_abort)()
{
    ParallelDescriptor::Abort();
}

#ifdef BL_USE_MPI
namespace ParallelDescriptor
{
#ifndef BL_AMRPROF
template <> MPI_Datatype Mpi_typemap<IntVect>::type()
{
    static MPI_Datatype mine(MPI_DATATYPE_NULL);
    if ( mine == MPI_DATATYPE_NULL )
    {
	IntVect iv[2];	// Used to construct the data types
	MPI_Datatype types[] = {
	    MPI_LB,
	    MPI_INT,
	    MPI_UB};
	int blocklens[] = { 1, BL_SPACEDIM, 1};
	MPI_Aint disp[3];
	int n = 0;
	BL_MPI_REQUIRE( MPI_Address(&iv[0],      &disp[n++]) );
	BL_MPI_REQUIRE( MPI_Address(&iv[0].vect, &disp[n++]) );
	BL_MPI_REQUIRE( MPI_Address(&iv[1],      &disp[n++]) );
	for ( int i = n-1; i >= 0; i-- )
	{
	    disp[i] -= disp[0];
	}
	BL_MPI_REQUIRE( MPI_Type_struct(n, blocklens, disp, types, &mine) );
	BL_MPI_REQUIRE( MPI_Type_commit( &mine ) );
    }
    return mine;
}

template <> MPI_Datatype Mpi_typemap<IndexType>::type()
{
    static MPI_Datatype mine(MPI_DATATYPE_NULL);
    if ( mine == MPI_DATATYPE_NULL )
    {
	IndexType iv[2];	// Used to construct the data types
	MPI_Datatype types[] = {
	    MPI_LB,
	    MPI_UNSIGNED,
	    MPI_UB};
	int blocklens[] = { 1, 1, 1};
	MPI_Aint disp[3];
	int n = 0;
	BL_MPI_REQUIRE( MPI_Address(&iv[0],       &disp[n++]) );
	BL_MPI_REQUIRE( MPI_Address(&iv[0].itype, &disp[n++]) );
	BL_MPI_REQUIRE( MPI_Address(&iv[1],       &disp[n++]) );
	for ( int i = n-1; i >= 0; i-- )
	{
	    disp[i] -= disp[0];
	}
	BL_MPI_REQUIRE( MPI_Type_struct(n, blocklens, disp, types, &mine) );
	BL_MPI_REQUIRE( MPI_Type_commit( &mine ) );
    }
    return mine;
}

template <> MPI_Datatype Mpi_typemap<Box>::type()
{
    static MPI_Datatype mine(MPI_DATATYPE_NULL);
    if ( mine == MPI_DATATYPE_NULL )
    {
	Box iv[2];	// Used to construct the data types
	MPI_Datatype types[] = {
	    MPI_LB,
	    Mpi_typemap<IntVect>::type(),
	    Mpi_typemap<IntVect>::type(),
	    Mpi_typemap<IndexType>::type(),
	    MPI_UB};
	int blocklens[] = { 1, 1, 1, 1, 1};
	MPI_Aint disp[5];
	int n = 0;
	BL_MPI_REQUIRE( MPI_Address(&iv[0],          &disp[n++]) );
	BL_MPI_REQUIRE( MPI_Address(&iv[0].smallend, &disp[n++]) );
	BL_MPI_REQUIRE( MPI_Address(&iv[0].bigend,   &disp[n++]) );
	BL_MPI_REQUIRE( MPI_Address(&iv[0].btype,    &disp[n++]) );
	BL_MPI_REQUIRE( MPI_Address(&iv[1],          &disp[n++]) );
	for ( int i = n-1; i >= 0; i-- )
	{
	    disp[i] -= disp[0];
	}
	BL_MPI_REQUIRE( MPI_Type_struct(n, blocklens, disp, types, &mine) );
	BL_MPI_REQUIRE( MPI_Type_commit( &mine ) );
    }
    return mine;
}
#endif
}
#endif

void
ParallelDescriptor::ReadAndBcastFile (const std::string& filename,
                                      Array<char>&       charBuf,
				      bool               bExitOnError,
				      const MPI_Comm    &comm)
{
    enum { IO_Buffer_Size = 40960 * 32 };

#ifdef BL_SETBUF_SIGNED_CHAR
    typedef signed char Setbuf_Char_Type;
#else
    typedef char Setbuf_Char_Type;
#endif

    Array<Setbuf_Char_Type> io_buffer(IO_Buffer_Size);

    int fileLength = 0, fileLengthPadded;

    std::ifstream iss;

    if (ParallelDescriptor::IOProcessor())
    {
        iss.rdbuf()->pubsetbuf(io_buffer.dataPtr(), io_buffer.size());
        iss.open(filename.c_str(), std::ios::in);
        if ( ! iss.good())
        {
	  if(bExitOnError) {
            BoxLib::FileOpenFailed(filename);
	  } else {
            fileLength = -1;
	  }
        } else {
          iss.seekg(0, std::ios::end);
          fileLength = iss.tellg();
          iss.seekg(0, std::ios::beg);
	}
    }
    ParallelDescriptor::Bcast(&fileLength, 1,
                              ParallelDescriptor::IOProcessorNumber(), comm);

    if(fileLength == -1) {
      return;
    }

    fileLengthPadded = fileLength + 1;
    fileLengthPadded += fileLengthPadded % 8;
    charBuf.resize(fileLengthPadded);
    if (ParallelDescriptor::IOProcessor())
    {
        iss.read(charBuf.dataPtr(), fileLength);
        iss.close();
    }
    ParallelDescriptor::Bcast(charBuf.dataPtr(), fileLengthPadded,
                              ParallelDescriptor::IOProcessorNumber(), comm);
    charBuf[fileLength] = '\0';
}


void
ParallelDescriptor::SidecarProcess ()
{
#ifdef IN_TRANSIT
#ifdef BL_USE_MPI
    bool finished(false);
    int signal(-1);
    while (!finished)
    {
        // Receive the signal from the compute group.
        ParallelDescriptor::Bcast(&signal, 1, 0, ParallelDescriptor::CommunicatorInter());

        // Process the  signal with the set of user-provided signal handlers
        for (ParallelDescriptor::SH_LIST::iterator it=The_Signal_Handler_List.begin();
             it != The_Signal_Handler_List.end() && signal != ParallelDescriptor::SidecarQuitSignal;
             ++it)
        {
          signal = (* *it)(signal);
        }

        if (signal == ParallelDescriptor::SidecarQuitSignal)
        {
            if (ParallelDescriptor::IOProcessor())
                std::cout << "Sidecars received the quit signal." << std::endl;
            finished = true;
            break;
        }
    }
    if (ParallelDescriptor::IOProcessor())
      std::cout << "===== SIDECARS DONE. EXITING ... =====" << std::endl;
#endif
#endif
}
