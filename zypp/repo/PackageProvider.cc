/*---------------------------------------------------------------------\
|                          ____ _   __ __ ___                          |
|                         |__  / \ / / . \ . \                         |
|                           / / \ V /|  _/  _/                         |
|                          / /__ | | | | | |                           |
|                         /_____||_| |_| |_|                           |
|                                                                      |
\---------------------------------------------------------------------*/
/** \file	zypp/repo/PackageProvider.cc
 *
*/
#include <iostream>
#include <fstream>
#include <sstream>
#include "zypp/repo/PackageDelta.h"
#include "zypp/base/Logger.h"
#include "zypp/base/Gettext.h"
#include "zypp/base/UserRequestException.h"
#include "zypp/base/NonCopyable.h"
#include "zypp/repo/PackageProvider.h"
#include "zypp/repo/Applydeltarpm.h"
#include "zypp/repo/PackageDelta.h"

#include "zypp/TmpPath.h"
#include "zypp/ZConfig.h"
#include "zypp/RepoInfo.h"
#include "zypp/RepoManager.h"

#include "zypp/ZYppFactory.h"
#include "zypp/Target.h"
#include "zypp/target/rpm/RpmDb.h"
#include "zypp/FileChecker.h"

using std::endl;

///////////////////////////////////////////////////////////////////
namespace zypp
{
  ///////////////////////////////////////////////////////////////////
  namespace repo
  {
    ///////////////////////////////////////////////////////////////////
    //	class PackageProviderPolicy
    ///////////////////////////////////////////////////////////////////

    bool PackageProviderPolicy::queryInstalled( const std::string & name_r,
                                                const Edition &     ed_r,
                                                const Arch &        arch_r ) const
    {
      if ( _queryInstalledCB )
        return _queryInstalledCB( name_r, ed_r, arch_r );
      return false;
    }


    ///////////////////////////////////////////////////////////////////
    /// \class PackageProvider::Impl
    /// \brief PackageProvider implementation.
    ///////////////////////////////////////////////////////////////////
    class PackageProvider::Impl : private base::NonCopyable
    {
      typedef callback::UserData UserData;
    public:
      /** Ctor taking the Package to provide. */
      Impl( RepoMediaAccess & access_r,
	    const Package::constPtr & package_r,
	    const DeltaCandidates & deltas_r,
	    const PackageProviderPolicy & policy_r )
      : _policy( policy_r )
      , _package( package_r )
      , _deltas( deltas_r )
      , _access( access_r )
      , _retry(false)
      {}

      virtual ~Impl() {}

      /** Factory method providing the appropriate implementation.
       * Called by PackageProvider ctor. Returned pointer should be
       * immediately wrapped into a smartpointer.
       */
      static Impl * factoryMake( RepoMediaAccess & access_r,
				 const Package::constPtr & package_r,
				 const DeltaCandidates & deltas_r,
				 const PackageProviderPolicy & policy_r );

    public:
      /** Provide the package.
       * The basic workflow.
       * \throws Exception.
       */
      ManagedFile providePackage() const;

      /** Provide the package if it is cached. */
      ManagedFile providePackageFromCache() const
      {
	ManagedFile ret( doProvidePackageFromCache() );
	if ( ! ( ret->empty() ||  _package->repoInfo().keepPackages() ) )
	  ret.setDispose( filesystem::unlink );
	return ret;
      }

      /** Whether the package is cached. */
      bool isCached() const
      { return ! doProvidePackageFromCache()->empty(); }

    protected:
      typedef PackageProvider::Impl	Base;
      typedef callback::SendReport<repo::DownloadResolvableReport>	Report;

      /** Lookup the final rpm in cache.
       *
       * A non empty ManagedFile will be returned to the caller.
       *
       * \note File disposal depending on the repos keepPackages setting
       * are not set here, but in \ref providePackage or \ref providePackageFromCache.
       *
       * \note The provoided default implementation returns an empty ManagedFile
       * (cache miss).
       */
      virtual ManagedFile doProvidePackageFromCache() const = 0;

      /** Actually provide the final rpm.
       * Report start/problem/finish and retry loop are hadled by \ref providePackage.
       * Here you trigger just progress and delta/plugin callbacks as needed.
       *
       * Proxy methods for progressPackageDownload and failOnChecksum are provided here.
       * Create similar proxies for other progress callbacks in derived classes and link
       * it to ProvideFilePolicy for download:
       * \code
       * ProvideFilePolicy policy;
       * policy.progressCB( bind( &Base::progressPackageDownload, this, _1 ) );
       * policy.failOnChecksumErrorCB( bind( &Base::failOnChecksumError, this ) );
       * return _access.provideFile( _package->repoInfo(), loc, policy );
       * \endcode
       *
       * \note The provoided default implementation retrieves the packages default
       * location.
       */
      virtual ManagedFile doProvidePackage() const = 0;

    protected:
      /** Access to the DownloadResolvableReport */
      Report & report() const
      { return *_report; }

      /** Redirect ProvideFilePolicy package download progress to this. */
      bool progressPackageDownload( int value ) const
      {	return report()->progress( value, _package ); }

      /** Redirect ProvideFilePolicy failOnChecksumError to this if needed. */
      bool failOnChecksumError() const
      {
	std::string package_str = _package->name() + "-" + _package->edition().asString();

	// TranslatorExplanation %s = package being checked for integrity
	switch ( report()->problem( _package, repo::DownloadResolvableReport::INVALID, str::form(_("Package %s seems to be corrupted during transfer. Do you want to retry retrieval?"), package_str.c_str() ) ) )
	{
	  case repo::DownloadResolvableReport::RETRY:
	    _retry = true;
	    break;
	  case repo::DownloadResolvableReport::IGNORE:
	    ZYPP_THROW(SkipRequestException("User requested skip of corrupted file"));
	    break;
	  case repo::DownloadResolvableReport::ABORT:
	    ZYPP_THROW(AbortRequestException("User requested to abort"));
	    break;
	  default:
	    break;
	}
	return true; // anyway a failure
      }

      typedef target::rpm::RpmDb RpmDb;

      RpmDb::CheckPackageResult packageSigCheck( const Pathname & path_r, UserData & userData ) const
      {
	if ( !_target )
	  _target = getZYpp()->getTarget();

	RpmDb::CheckPackageResult ret = RpmDb::CHK_ERROR;
	RpmDb::CheckPackageDetail detail;
	if ( _target )
	  ret = _target->rpmDb().checkPackage( path_r, detail );
	else
	  detail.push_back( RpmDb::CheckPackageDetail::value_type( ret, "OOps. Target is not initialized!" ) );

	userData.set( "CheckPackageResult", ret );
	userData.set( "CheckPackageDetail", std::move(detail) );
	return ret;
      }

      /** React on signature verrification error user action */
      void resolveSignatureErrorAction( repo::DownloadResolvableReport::Action action_r ) const
      {
	// TranslatorExplanation %s = package being checked for integrity
	switch ( action_r )
	{
	  case repo::DownloadResolvableReport::RETRY:
	    _retry = true;
	    break;
	  case repo::DownloadResolvableReport::IGNORE:
	    WAR << _package->asUserString() << ": " << "User requested skip of insecure file" << endl;
	    break;
	  default:
	  case repo::DownloadResolvableReport::ABORT:
	    ZYPP_THROW(AbortRequestException("User requested to abort"));
	    break;
	}
      }

      /** Default signature verrification error handling. */
      void defaultReportSignatureError( RpmDb::CheckPackageResult ret, const std::string & detail_r = std::string() ) const
      {
	str::Str msg;
	msg << _package->asUserString() << ": " << _("Signature verification failed") << " " << ret;
	if ( ! detail_r.empty() )
	  msg << "\n" << detail_r;
	resolveSignatureErrorAction( report()->problem( _package, repo::DownloadResolvableReport::INVALID, msg.str() ) );
      }

    protected:
      PackageProviderPolicy	_policy;
      Package::constPtr		_package;
      DeltaCandidates		_deltas;
      RepoMediaAccess &		_access;

    private:
      typedef shared_ptr<void>	ScopedGuard;

      ScopedGuard newReport() const
      {
	_report.reset( new Report );
	// Use a custom deleter calling _report.reset() when guard goes out of
	// scope (cast required as reset is overloaded). We want report to end
	// when leaving providePackage and not wait for *this going out of scope.
	return shared_ptr<void>( static_cast<void*>(0),
				 bind( mem_fun_ref( static_cast<void (shared_ptr<Report>::*)()>(&shared_ptr<Report>::reset) ),
				       ref(_report) ) );
      }

      mutable bool               _retry;
      mutable shared_ptr<Report> _report;
      mutable Target_Ptr         _target;
    };
    ///////////////////////////////////////////////////////////////////

    /** Default implementation (cache miss). */
    ManagedFile PackageProvider::Impl::doProvidePackageFromCache() const
    { return ManagedFile(); }

    /** Default implementation (provide full package) */
    ManagedFile PackageProvider::Impl::doProvidePackage() const
    {
      ManagedFile ret;
      OnMediaLocation loc = _package->location();

      ProvideFilePolicy policy;
      policy.progressCB( bind( &Base::progressPackageDownload, this, _1 ) );
      policy.failOnChecksumErrorCB( bind( &Base::failOnChecksumError, this ) );
      return _access.provideFile( _package->repoInfo(), loc, policy );
    }

    ///////////////////////////////////////////////////////////////////

    ManagedFile PackageProvider::Impl::providePackage() const
    {
      ScopedGuard guardReport( newReport() );

      // check for cache hit:
      ManagedFile ret( providePackageFromCache() );
      if ( ! ret->empty() )
      {
	MIL << "provided Package from cache " << _package << " at " << ret << endl;
	report()->infoInCache( _package, ret );
	return ret; // <-- cache hit
      }

      // HERE: cache misss, check toplevel cache or do download:
      RepoInfo info = _package->repoInfo();

      // Check toplevel cache
      {
	RepoManagerOptions topCache;
	if ( info.packagesPath().dirname() != topCache.repoPackagesCachePath )	// not using toplevel cache
	{
	  const OnMediaLocation & loc( _package->location() );
	  if ( ! loc.checksum().empty() )	// no cache hit without checksum
	  {
	    PathInfo pi( topCache.repoPackagesCachePath / info.packagesPath().basename() / loc.filename() );
	    if ( pi.isExist() && loc.checksum() == CheckSum( loc.checksum().type(), std::ifstream( pi.c_str() ) ) )
	    {
	      report()->start( _package, pi.path().asFileUrl() );
	      const Pathname & dest( info.packagesPath() / loc.filename() );
	      if ( filesystem::assert_dir( dest.dirname() ) == 0 && filesystem::hardlinkCopy( pi.path(), dest ) == 0 )
	      {
		ret = ManagedFile( dest );
		if ( ! info.keepPackages() )
		  ret.setDispose( filesystem::unlink );

		MIL << "provided Package from toplevel cache " << _package << " at " << ret << endl;
		report()->finish( _package, repo::DownloadResolvableReport::NO_ERROR, std::string() );
		return ret; // <-- toplevel cache hit
	      }
	    }
	  }
	}
      }

      // FIXME we only support the first url for now.
      if ( info.baseUrlsEmpty() )
        ZYPP_THROW(Exception("No url in repository."));

      MIL << "provide Package " << _package << endl;
      Url url = * info.baseUrlsBegin();
      do {
        _retry = false;
	if ( ! ret->empty() )
	{
	  ret.setDispose( filesystem::unlink );
	  ret.reset();
	}
        report()->start( _package, url );
        try
          {
            ret = doProvidePackage();

	    if ( info.pkgGpgCheck() )
	    {
	      UserData userData( "pkgGpgCheck" );
	      userData.set( "Package", _package );
	      userData.set( "Localpath", ret.value() );
	      RpmDb::CheckPackageResult res = packageSigCheck( ret, userData );
	      // publish the checkresult, even if it is OK. Apps may want to report something...
	      report()->pkgGpgCheck( userData );

	      if ( res != RpmDb::CHK_OK )
	      {
		if ( userData.hasvalue( "Action" ) )	// pkgGpgCheck report provided an user error action
		{
		  resolveSignatureErrorAction( userData.get( "Action", repo::DownloadResolvableReport::ABORT ) );
		}
		else if ( userData.haskey( "Action" ) )	// pkgGpgCheck requests the default problem report (wo. details)
		{
		  defaultReportSignatureError( res );
		}
		else					// no advice from user => usedefaults
		{
		  switch ( res )
		  {
		    case RpmDb::CHK_OK:		// Signature is OK
		      break;

		    case RpmDb::CHK_NOKEY:	// Public key is unavailable
		    case RpmDb::CHK_NOTFOUND:	// Signature is unknown type
		    case RpmDb::CHK_FAIL:	// Signature does not verify
		    case RpmDb::CHK_NOTTRUSTED:	// Signature is OK, but key is not trusted
		    case RpmDb::CHK_ERROR:	// File does not exist or can't be opened
		    default:
		      // report problem (w. details), throw if to abort, else retry/ignore
		      defaultReportSignatureError( res, str::Str() << userData.get<RpmDb::CheckPackageDetail>( "CheckPackageDetail" ) );
		      break;
		  }
		}
	      }
	    }
          }
        catch ( const UserRequestException & excpt )
          {
            // UserRequestException e.g. from failOnChecksumError was already reported.
            ERR << "Failed to provide Package " << _package << endl;
            if ( ! _retry )
              {
                ZYPP_RETHROW( excpt );
              }
          }
        catch ( const Exception & excpt )
          {
            ERR << "Failed to provide Package " << _package << endl;
            if ( ! _retry )
              {
                // Aything else gets reported
                std::string package_str = _package->name() + "-" + _package->edition().asString();

                // TranslatorExplanation %s = name of the package being processed.
                std::string detail_str( str::form(_("Failed to provide Package %s. Do you want to retry retrieval?"), package_str.c_str() ) );
                detail_str += str::form( "\n\n%s", excpt.asUserHistory().c_str() );

                switch ( report()->problem( _package, repo::DownloadResolvableReport::IO, detail_str.c_str() ) )
                {
                      case repo::DownloadResolvableReport::RETRY:
                        _retry = true;
                        break;
                      case repo::DownloadResolvableReport::IGNORE:
                        ZYPP_THROW(SkipRequestException("User requested skip of corrupted file", excpt));
                        break;
                      case repo::DownloadResolvableReport::ABORT:
                        ZYPP_THROW(AbortRequestException("User requested to abort", excpt));
                        break;
                      default:
                        ZYPP_RETHROW( excpt );
                        break;
                }
              }
          }
      } while ( _retry );

      report()->finish( _package, repo::DownloadResolvableReport::NO_ERROR, std::string() );
      MIL << "provided Package " << _package << " at " << ret << endl;
      return ret;
    }


    ///////////////////////////////////////////////////////////////////
    /// \class RpmPackageProvider
    /// \brief RPM PackageProvider implementation.
    ///////////////////////////////////////////////////////////////////
    class RpmPackageProvider : public PackageProvider::Impl
    {
    public:
      RpmPackageProvider( RepoMediaAccess & access_r,
			  const Package::constPtr & package_r,
			  const DeltaCandidates & deltas_r,
			  const PackageProviderPolicy & policy_r )
      : PackageProvider::Impl( access_r, package_r, deltas_r, policy_r )
      {}

    protected:
      virtual ManagedFile doProvidePackageFromCache() const;

      virtual ManagedFile doProvidePackage() const;

    private:
      typedef packagedelta::DeltaRpm	DeltaRpm;

      ManagedFile tryDelta( const DeltaRpm & delta_r ) const;

      bool progressDeltaDownload( int value ) const
      { return report()->progressDeltaDownload( value ); }

      void progressDeltaApply( int value ) const
      { return report()->progressDeltaApply( value ); }

      bool queryInstalled( const Edition & ed_r = Edition() ) const
      { return _policy.queryInstalled( _package->name(), ed_r, _package->arch() ); }
    };
    ///////////////////////////////////////////////////////////////////

    ManagedFile RpmPackageProvider::doProvidePackageFromCache() const
    {
      return ManagedFile( _package->cachedLocation() );
    }

    ManagedFile RpmPackageProvider::doProvidePackage() const
    {
      Url url;
      RepoInfo info = _package->repoInfo();
      // FIXME we only support the first url for now.
      if ( info.baseUrlsEmpty() )
        ZYPP_THROW(Exception("No url in repository."));
      else
        url = * info.baseUrlsBegin();

      // check whether to process patch/delta rpms
      if ( ZConfig::instance().download_use_deltarpm()
	&& ( url.schemeIsDownloading() || ZConfig::instance().download_use_deltarpm_always() ) )
      {
	std::list<DeltaRpm> deltaRpms;
	_deltas.deltaRpms( _package ).swap( deltaRpms );

	if ( ! deltaRpms.empty() && queryInstalled() && applydeltarpm::haveApplydeltarpm() )
	{
	  for_( it, deltaRpms.begin(), deltaRpms.end())
	  {
	    DBG << "tryDelta " << *it << endl;
	    ManagedFile ret( tryDelta( *it ) );
	    if ( ! ret->empty() )
	      return ret;
	  }
	}
      }

      // no patch/delta -> provide full package
      return Base::doProvidePackage();
    }

    ManagedFile RpmPackageProvider::tryDelta( const DeltaRpm & delta_r ) const
    {
      if ( delta_r.baseversion().edition() != Edition::noedition
           && ! queryInstalled( delta_r.baseversion().edition() ) )
        return ManagedFile();

      if ( ! applydeltarpm::quickcheck( delta_r.baseversion().sequenceinfo() ) )
        return ManagedFile();

      report()->startDeltaDownload( delta_r.location().filename(),
                                    delta_r.location().downloadSize() );
      ManagedFile delta;
      try
        {
          ProvideFilePolicy policy;
          policy.progressCB( bind( &RpmPackageProvider::progressDeltaDownload, this, _1 ) );
          delta = _access.provideFile( delta_r.repository().info(), delta_r.location(), policy );
        }
      catch ( const Exception & excpt )
        {
          report()->problemDeltaDownload( excpt.asUserHistory() );
          return ManagedFile();
        }
      report()->finishDeltaDownload();

      report()->startDeltaApply( delta );
      if ( ! applydeltarpm::check( delta_r.baseversion().sequenceinfo() ) )
        {
          report()->problemDeltaApply( _("applydeltarpm check failed.") );
          return ManagedFile();
        }

      // build the package and put it into the cache
      Pathname destination( _package->repoInfo().packagesPath() / _package->location().filename() );

      if ( ! applydeltarpm::provide( delta, destination,
                                     bind( &RpmPackageProvider::progressDeltaApply, this, _1 ) ) )
        {
          report()->problemDeltaApply( _("applydeltarpm failed.") );
          return ManagedFile();
        }
      report()->finishDeltaApply();

      return ManagedFile( destination, filesystem::unlink );
    }

#if 0
    ///////////////////////////////////////////////////////////////////
    /// \class PluginPackageProvider
    /// \brief Plugin PackageProvider implementation.
    ///
    /// Basically downloads the default package and calls a
    /// 'stem'2rpm plugin to cteate the final .rpm package.
    ///////////////////////////////////////////////////////////////////
    class PluginPackageProvider : public PackageProvider::Impl
    {
    public:
      PluginPackageProvider( const std::string & stem_r,
			     RepoMediaAccess & access_r,
			     const Package::constPtr & package_r,
			     const DeltaCandidates & deltas_r,
			     const PackageProviderPolicy & policy_r )
      : Base( access_r, package_r, deltas_r, policy_r )
      {}

    protected:
      virtual ManagedFile doProvidePackageFromCache() const
      {
	return Base::doProvidePackageFromCache();
      }

      virtual ManagedFile doProvidePackage() const
      {
	return Base::doProvidePackage();
      }
    };
    ///////////////////////////////////////////////////////////////////
#endif

    ///////////////////////////////////////////////////////////////////
    //	class PackageProvider
    ///////////////////////////////////////////////////////////////////

    PackageProvider::Impl * PackageProvider::Impl::factoryMake( RepoMediaAccess & access_r,
								const Package::constPtr & package_r,
								const DeltaCandidates & deltas_r,
								const PackageProviderPolicy & policy_r )
    {
      return new RpmPackageProvider( access_r, package_r, deltas_r, policy_r );
    }

    PackageProvider::PackageProvider( RepoMediaAccess & access_r,
				      const Package::constPtr & package_r,
				      const DeltaCandidates & deltas_r,
				      const PackageProviderPolicy & policy_r )
    : _pimpl( Impl::factoryMake( access_r, package_r, deltas_r, policy_r ) )
    {}

    PackageProvider::~PackageProvider()
    {}

    ManagedFile PackageProvider::providePackage() const
    { return _pimpl->providePackage(); }

    ManagedFile PackageProvider::providePackageFromCache() const
    { return _pimpl->providePackageFromCache(); }

    bool PackageProvider::isCached() const
    { return _pimpl->isCached(); }

  } // namespace repo
  ///////////////////////////////////////////////////////////////////
} // namespace zypp
///////////////////////////////////////////////////////////////////
