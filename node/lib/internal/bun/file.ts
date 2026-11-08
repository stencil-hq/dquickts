// internal:bun/file — Bun.file(), Bun.write(), Bun.stdin/stdout/stderr, FileSink, Bun.mmap().
//
// A BunFile is a lazy reference to a path or file descriptor (optionally a byte
// window of it): nothing is opened until a read, stat or write. Its prototype is
// chained to the global Blob (node:buffer) the first time a BunFile is used, so
// `file instanceof Blob` holds and Blob consumers (new Blob([file]), Response)
// can read its bytes. node:fs and node:buffer are loaded lazily.

import * as core from "qbun:core";
import * as text from "qbun:text";
import * as fs from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, extname } from "node:path";
import { codedError, invalidArgType, outOfRange } from "internal:errors";
import type { Stats } from "node:fs";
import type { FileHandle } from "node:fs/promises";

interface QbunText {
	utf8Encode(s: string): Uint8Array;
	byteLengthUtf8(s: string): number;
	decode(bytes: Uint8Array, encoding: string, start: number, end: number): string;
}
const textNative = text as unknown as QbunText;

interface QbunCoreIo {
	writeSync(fd: number, data: string | Uint8Array): number;
}
const coreIo = core as unknown as QbunCoreIo;

const CHUNK = 64 * 1024;
const DEFAULT_SINK_HWM = 64 * 1024;
const DEFAULT_TYPE = "application/octet-stream";
const MAX_FD = 2147483647;

// ── MIME types (Bun's extension table, src/http_types/MimeType.rs) ──────────
// Encoded as "|"-separated "<mime> <ext> <ext>..." entries; prefixes:
// a/ application/, t/ text/, i/ image/, v/ video/, u/ audio/, f/ font/,
// m/ model/, g/ message/, c/ chemical/, xc/ x-conference/; "~" stands for "vnd.".
const MIME_TABLE =
	"a/~lotus-1-2-3 123|a/~1000minds.decision-model+xml 1km|t/~in3d.3dml 3dml|i/x-3ds 3ds|v/3gpp2 3g2|" +
	"v/3gpp 3gp 3gpp|m/3mf 3mf|a/x-7z-compressed 7z|a/x-authorware-bin aab u32 vox x32|u/x-aac aac|" +
	"a/x-authorware-map aam|a/x-authorware-seg aas|a/x-abiword abw|a/~nokia.n-gage.ac+xml ac|" +
	"a/~americandynamics.acc acc|a/x-ace-compressed ace|a/~acucobol acu|a/~acucorp acutc atc|u/adpcm adp|" +
	"a/~audiograph aep|a/x-font-type1 afm pfa pfb pfm|a/~ibm.modcap afp list3820 listafp|a/~age age|" +
	"a/~ahead.space ahead|a/postscript ai eps ps|u/x-aiff aif aifc aiff|" +
	"a/~adobe.air-application-installer-package+zip air|a/~dvb.ait ait|a/~amiga.ami ami|u/amr amr|" +
	"a/~android.package-archive apk|i/apng apng|t/cache-manifest appcache manifest|a/x-ms-application application|" +
	"a/~lotus-approach apr|a/x-freearc arc|a/x-arj arj|a/pgp-signature asc sig|v/x-ms-asf asf asx|t/x-asm asm s|" +
	"a/~accpac.simply.aso aso|a/atom+xml atom|a/atomcat+xml atomcat|a/atomdeleted+xml atomdeleted|" +
	"a/atomsvc+xml atomsvc|a/~antix.game-component atx|u/basic au snd|i/avci avci|i/avcs avcs|v/x-msvideo avi|" +
	"i/avif avif|a/applixware aw|a/~airzip.filesecure.azf azf|a/~airzip.filesecure.azs azs|" +
	"i/~airzip.accelerator.azv azv|a/~amazon.ebook azw|i/~pco.b16 b16|a/x-msdownload bat com dll exe msi|" +
	"a/x-bcpio bcpio|a/x-font-bdf bdf|a/~syncml.dm+wbxml bdm|a/x-bdoc bdoc|a/~realvnc.bed bed|" +
	"a/~fujitsu.oasysprs bh2|" +
	"a/octet-stream bin bpk buffer deploy dist distz dms dump elc img lrf mar msm msp pkg so|a/x-blorb blb blorb|" +
	"a/~bmi bmi|a/~balsamiq.bmml+xml bmml|i/x-ms-bmp bmp|a/~framemaker book fm frame maker|" +
	"a/~previewsystems.box box|a/x-bzip2 boz bz2|m/~valve.source.compiled-map bsp|i/prs.btif btif|a/x-bzip bz|" +
	"t/x-c c cc cpp cxx dic h hh|a/~cluetrust.cartomobile-config c11amc|a/~cluetrust.cartomobile-config-pkg c11amz|" +
	"a/~clonk.c4group c4d c4f c4g c4p c4u|a/~ms-cab-compressed cab|u/x-caf caf|a/~tcpdump.pcap cap dmp pcap|" +
	"a/~curl.car car|a/~ms-pki.seccat cat|a/x-cbr cb7 cba cbr cbt cbz|a/x-cocoa cco|" +
	"a/x-director cct cst cxt dcr dir dxr fgd swa w3d|a/ccxml+xml ccxml|a/~contact.cmsg cdbcmsg|a/x-netcdf cdf nc|" +
	"a/cdfx+xml cdfx|a/~mediastation.cdkey cdkey|a/cdmi-capability cdmia|a/cdmi-container cdmic|a/cdmi-domain cdmid|" +
	"a/cdmi-object cdmio|a/cdmi-queue cdmiq|c/x-cdx cdx|a/~chemdraw+xml cdxml|a/~cinderella cdy|a/pkix-cert cer|" +
	"a/x-cfs-compressed cfs|i/cgm cgm|a/x-chat chat|a/~ms-htmlhelp chm|a/~kde.kchart chrt|c/x-cif cif|" +
	"a/~anser-web-certificate-issue-initiation cii|a/~ms-artgalry cil|a/javascript cjs cts js mjs mts mtsx ts tsx|" +
	"a/~claymore cla|a/java-vm class|a/~crick.clicker.keyboard clkk|a/~crick.clicker.palette clkp|" +
	"a/~crick.clicker.template clkt|a/~crick.clicker.wordbank clkw|a/~crick.clicker clkx|a/x-msclip clp|" +
	"a/~cosmocaller cmc|c/x-cmdf cmdf|c/x-cml cml|a/~yellowriver-custom-menu cmp|i/x-cmx cmx|a/~rim.cod cod|" +
	"t/coffeescript coffee litcoffee|t/plain conf def in ini list log text txt|a/x-cpio cpio|a/cpl+xml cpl|" +
	"a/mac-compactpro cpt|a/x-mscardfile crd|a/pkix-crl crl|a/x-x509-ca-cert crt der pem|a/x-chrome-extension crx|" +
	"a/~rig.cryptonote cryptonote|a/x-csh csh|a/~citationstyles.style+xml csl|c/x-csml csml|a/~commonspace csp|" +
	"t/css css|t/csv csv|a/cu-seeme cu|t/~curl curl|a/prs.cww cww|m/~collada+xml dae|a/~mobius.daf daf|a/~dart dart|" +
	"a/~fdsn.seed dataless seed|a/davmount+xml davmount|a/~dbf dbf|a/docbook+xml dbk|t/~curl.dcurl dcurl|" +
	"a/~oma.dd2+xml dd2|a/~fujixerox.ddd ddd|a/~syncml.dmddf+xml ddf|i/~ms-dds dds|a/x-debian-package deb udeb|" +
	"a/~dreamfactory dfac|a/x-dgc-compressed dgc|a/~mobius.dis dis|g/disposition-notification disposition-n|" +
	"i/~djvu djv djvu|a/x-apple-diskimage dmg|a/~dna dna|a/msword doc dot|a/~ms-word.document.macroenabled.12 docm|" +
	"a/~openxmlformats-officedocument.wordprocessingml.document docx|a/~ms-word.template.macroenabled.12 dotm|" +
	"a/~openxmlformats-officedocument.wordprocessingml.template dotx|a/~osgi.dp dp|a/~dpgraph dpg|u/~dra dra|" +
	"i/dicom-rle drle|t/prs.lines.tag dsc|a/dssc+der dssc|a/x-dtbook+xml dtb|a/xml-dtd dtd|u/~dts dts|" +
	"u/~dts.hd dtshd|v/~dvb.file dvb|a/x-dvi dvi|a/atsc-dwd+xml dwd|m/~dwf dwf|i/~dwg dwg|i/~dxf dxf|" +
	"a/~spotfire.dxp dxp|a/java-archive ear jar war|u/~nuera.ecelp4800 ecelp4800|u/~nuera.ecelp7470 ecelp7470|" +
	"u/~nuera.ecelp9600 ecelp9600|a/ecmascript ecma es|a/~novadigm.edm edm|a/~novadigm.edx edx|a/~picsel efif|" +
	"a/~pg.osasli ei6|i/emf emf|g/rfc822 eml mime|a/emma+xml emma|a/emotionml+xml emotionml|a/x-msmetafile emz wmz|" +
	"u/~digital-winds eol|a/~ms-fontobject eot|a/epub+zip epub|a/~eszigno3+xml es3 et3|a/~osgi.subsystem esa|" +
	"a/~epson.esf esf|t/x-setext etx|a/x-eva eva|a/x-envoy evy|a/exi exi|a/express exp|i/aces exr|" +
	"a/~novadigm.ext ext|a/andrew-inset ez|a/~ezpix-album ez2|a/~ezpix-package ez3|t/x-fortran f f77 f90 for|" +
	"v/x-f4v f4v|i/~fastbidsheet fbs|a/~adobe.formscentral.fcdt fcdt|a/~isac.fcs fcs|a/~fdf fdf|a/fdt+xml fdt|" +
	"a/~denovo.fcselayout-link fe_launch|a/~fujitsu.oasysgp fg5|i/x-freehand fh fh4 fh5 fh7 fhc|a/x-xfig fig|" +
	"i/fits fits|u/x-flac flac|v/x-fli fli|a/~micrografx.flo flo|v/x-flv flv|a/~kde.kivio flw|t/~fmi.flexstor flx|" +
	"t/~fly fly|a/~frogans.fnc fnc|a/~software602.filler.form+xml fo|i/~fpx fpx|a/~fsc.weblaunch fsc|i/~fst fst|" +
	"a/~fluxtime.clip ftc|a/~anser-web-funds-transfer-initiation fti|v/~fvt fvt|a/~adobe.fxp fxp fxpl|" +
	"a/~fuzzysheet fzs|a/~geoplan g2w|i/g3fax g3|a/~geospace g3w|a/~groove-account gac|a/x-tads gam|" +
	"a/rpki-ghostbusters gbr|a/x-gca-compressed gca|m/~gdl gdl|a/~google-apps.document gdoc|" +
	"t/~familysearch.gedcom ged|a/~dynageo geo|a/geo+json geojson|a/~geometry-explorer gex gre|a/~geogebra.file ggb|" +
	"a/~geogebra.tool ggt|a/~groove-help ghf|i/gif gif|a/~groove-identity-message gim|m/gltf-binary glb|" +
	"m/gltf+json gltf|a/gml+xml gml|a/~gmx gmx|a/x-gnumeric gnumeric|a/~flographit gph|a/gpx+xml gpx|" +
	"a/~grafeq gqf gqs|a/srgs gram|a/x-gramps-xml gramps|a/~groove-injector grv|a/srgs+xml grxml|" +
	"a/x-font-ghostscript gsf|a/~google-apps.spreadsheet gsheet|a/~google-apps.presentation gslides|a/x-gtar gtar|" +
	"a/~groove-tool-message gtm|m/~gtw gtw|t/~graphviz gv|a/gxf gxf|a/~geonext gxt|a/gzip gz|v/h261 h261|" +
	"v/h263 h263|v/h264 h264|a/~hal+xml hal|a/~hbci hbci|t/x-handlebars-template hbs|a/x-virtualbox-hdd hdd|" +
	"a/x-hdf hdf|i/heic heic|i/heic-sequence heics|i/heif heif|i/heif-sequence heifs|i/hej2k hej2|" +
	"a/atsc-held+xml held|a/hjson hjson|a/winhlp hlp|a/~hp-hpgl hpgl|a/~hp-hpid hpid|a/~hp-hps hps|" +
	"a/mac-binhex40 hqx|i/hsj2 hsj2|t/x-component htc|a/~kenameaapp htke|t/html htm html shtml|a/~yamaha.hv-dic hvd|" +
	"a/~yamaha.hv-voice hvp|a/~yamaha.hv-script hvs|a/~intergeo i2g|a/~iccprofile icc icm|xc/x-cooltalk ice|" +
	"i/x-icon ico|t/calendar ics ifb|i/ief ief|a/~shana.informed.formdata ifm|m/iges iges igs|a/~igloader igl|" +
	"a/~insors.igm igm|a/~micrografx.igx igx|a/~shana.informed.interchange iif|a/~accpac.simply.imp imp|" +
	"a/~ms-ims ims|a/inkml+xml ink inkml|a/x-install-instructions install|a/~astraea-software.iota iota|" +
	"a/ipfix ipfix|a/~shana.informed.package ipk|a/~ibm.rights-management irm|a/~irepository.package+xml irp|" +
	"a/x-iso9660-image iso|a/~shana.informed.formtemplate itp|a/its+xml its|a/~immervision-ivp ivp|" +
	"a/~immervision-ivu ivu|t/~sun.j2me.app-descriptor jad|t/jade jade|a/~jam jam|a/x-java-archive-diff jardiff|" +
	"t/x-java-source java|i/jphc jhc|a/~jisp jisp|i/jls jls|a/~hp-jlyt jlt|i/x-jng jng|a/x-java-jnlp-file jnlp|" +
	"a/~joost.joda-archive joda|i/jp2 jp2 jpg2|i/jpeg jpe jpeg jpg|i/jpx jpf jpx|v/jpm jpgm jpm|v/jpeg jpgv|" +
	"i/jph jph|a/json json map|a/json5 json5|a/ld+json jsonld|a/jsonml+json jsonml|t/jsx jsx|i/jxr jxr|i/jxra jxra|" +
	"i/jxrs jxrs|i/jxs jxs|i/jxsc jxsc|i/jxsi jxsi|i/jxss jxss|u/midi kar mid midi rmi|a/~kde.karbon karbon|" +
	"a/x-keepass2 kdbx|a/x-iwork-keynote-sffkey key|a/~kde.kformula kfo|a/~kidspiration kia|" +
	"a/~google-earth.kml+xml kml|a/~google-earth.kmz kmz|a/~kinar kne knp|a/~kde.kontour kon|" +
	"a/~kde.kpresenter kpr kpt|a/~ds-keypoint kpxx|a/~kde.kspread ksp|a/~kahootz ktr ktz|i/ktx ktx|i/ktx2 ktx2|" +
	"a/~kde.kword kwd kwt|a/~las.las+xml lasxml|a/x-latex latex|a/~llamagraphics.life-balance.desktop lbd|" +
	"a/~llamagraphics.life-balance.exchange+xml lbe|a/~hhe.lesson-player les|t/less less|a/lgr+xml lgr|" +
	"a/x-lzh-compressed lha lzh|a/~route66.link66+xml link66|a/x-ms-shortcut lnk|a/lost+xml lostxml|a/~ms-lrm lrm|" +
	"a/~frogans.ltf ltf|t/x-lua lua|a/x-lua-bytecode luac|u/~lucent.voice lvp|a/~lotus-wordpro lwp|" +
	"a/x-msmediaview m13 m14 mvb|v/mpeg m1v m2v mpe mpeg mpg|a/mp21 m21 mp21|u/mpeg m2a m3a mp2 mp2a mp3 mpga|" +
	"u/x-mpegurl m3u|a/~apple.mpegurl m3u8|u/x-m4a m4a|a/mp4 m4p mp4s|v/iso.segment m4s|v/~mpegurl m4u mxu|" +
	"v/x-m4v m4v|a/mathematica ma mb nb|a/mads+xml mads|a/mmt-aei+xml maei|a/~ecowin.chart mag|" +
	"t/troff man me ms roff t tr|t/markdown markdown md|a/mathml+xml mathml|a/~mobius.mbk mbk|a/mbox mbox|" +
	"a/~medcalcdata mc1|a/~mcd mcd|t/~curl.mcurl mcurl|a/x-msaccess mdb|i/~ms-modi mdi|t/mdx mdx|" +
	"m/mesh mesh msh silo|a/metalink4+xml meta4|a/metalink+xml metalink|a/mets+xml mets|a/~mfmp mfm|" +
	"a/rpki-manifest mft|a/~osgeo.mapguide.package mgp|a/~proteus.magazine mgz|a/x-mie mie|a/~mif mif|" +
	"v/mj2 mj2 mjp2|v/x-matroska mk3d mks mkv|u/x-matroska mka|t/x-markdown mkd|a/~dolby.mlp mlp|" +
	"a/~chipnuts.karaoke-mmd mmd|a/~smaf mmf|t/mathml mml|i/~fujixerox.edmics-mmr mmr|v/x-mng mng|a/x-msmoney mny|" +
	"a/x-mobipocket-ebook mobi|a/mods+xml mods|v/quicktime mov qt|v/x-sgi-movie movie|v/mp4 mp4 mp4v mpg4|" +
	"u/mp4 mp4a|a/~mophun.certificate mpc|a/dash+xml mpd|a/media-policy-dataset+xml mpf|a/~apple.installer+xml mpkg|" +
	"a/~blueice.multipass mpm|a/~mophun.application mpn|a/~ms-project mpp mpt|a/~ibm.minipay mpy|a/~mobius.mqy mqy|" +
	"a/marc mrc|a/marcxml+xml mrcx|a/mediaservercontrol+xml mscml|a/~fdsn.mseed mseed|a/~mseq mseq|a/~epson.msf msf|" +
	"a/~ms-outlook msg|a/~mobius.msl msl|a/~muvee.style msty|m/mtl mtl|a/~musician mus|a/mmt-usd+xml musd|" +
	"a/~recordare.musicxml+xml musicxml|a/~mapbox-vector-tile mvt|a/~mfer mwf|a/mxf mxf|a/~recordare.musicxml mxl|" +
	"u/mobile-xmf mxmf|a/xv+xml mxml xhvml xvm xvml|a/~triscape.mxs mxs|a/~nokia.n-gage.symbian.install n-g|t/n3 n3|" +
	"a/~wolfram.player nbp|a/x-dtbncx+xml ncx|t/x-nfo nfo|a/~nokia.n-gage.data ngdat|a/~nitf nitf ntf|" +
	"a/~neurolanguage.nlu nlu|a/~enliven nml|a/~noblenet-directory nnd|a/~noblenet-sealer nns|a/~noblenet-web nnw|" +
	"i/~net-fpx npx|a/n-quads nq|a/x-conference nsc|a/~lotus-notes nsf|a/n-triples nt|" +
	"a/x-iwork-numbers-sffnumbers numbers|a/x-nzb nzb|a/~fujitsu.oasys2 oa2|a/~fujitsu.oasys3 oa3|" +
	"a/~fujitsu.oasys oas|a/x-msbinder obd|a/~openblox.game+xml obgx|m/obj obj|a/oda oda|" +
	"a/~oasis.opendocument.database odb|a/~oasis.opendocument.chart odc|a/~oasis.opendocument.formula odf|" +
	"a/~oasis.opendocument.formula-template odft|a/~oasis.opendocument.graphics odg|a/~oasis.opendocument.image odi|" +
	"a/~oasis.opendocument.text-master odm|a/~oasis.opendocument.presentation odp|" +
	"a/~oasis.opendocument.spreadsheet ods|a/~oasis.opendocument.text odt|u/ogg oga ogg opus spx|m/~opengex ogex|" +
	"v/ogg ogv|a/ogg ogx|a/omdoc+xml omdoc|a/onenote onepkg onetmp onetoc onetoc2|a/oebps-package+xml opf|" +
	"t/x-opml opml|a/~palm oprc pqa|t/x-org org|a/~yamaha.openscoreformat osf|" +
	"a/~yamaha.openscoreformat.osfpvg+xml osfpvg|a/~openstreetmap.data+xml osm|" +
	"a/~oasis.opendocument.chart-template otc|f/otf otf|a/~oasis.opendocument.graphics-template otg|" +
	"a/~oasis.opendocument.text-web oth|a/~oasis.opendocument.image-template oti|" +
	"a/~oasis.opendocument.presentation-template otp|a/~oasis.opendocument.spreadsheet-template ots|" +
	"a/~oasis.opendocument.text-template ott|a/x-virtualbox-ova ova|a/x-virtualbox-ovf ovf|a/rdf+xml owl rdf|" +
	"a/oxps oxps|a/~openofficeorg.extension oxt|t/x-pascal p pas|a/pkcs10 p10|a/x-pkcs12 p12 pfx|" +
	"a/x-pkcs7-certificates p7b spc|a/pkcs7-mime p7c p7m|a/x-pkcs7-certreqresp p7r|a/pkcs7-signature p7s|a/pkcs8 p8|" +
	"a/x-ns-proxy-autoconfig pac|a/x-iwork-pages-sffpages pages|a/~pawaafile paw|a/~powerbuilder6 pbd|" +
	"i/x-portable-bitmap pbm|a/x-font-pcf pcf|a/~hp-pcl pcl|a/~hp-pclxl pclxl|i/x-pict pct pic|a/~curl.pcurl pcurl|" +
	"i/x-pcx pcx|a/x-pilot pdb|t/x-processing pde|a/pdf pdf|a/font-tdpfr pfr|i/x-portable-graymap pgm|" +
	"a/x-chess-pgn pgn|a/pgp-encrypted pgp|a/x-httpd-php php|a/pkixcmp pki|a/pkix-pkipath pkipath|" +
	"a/~apple.pkpass pkpass|a/x-perl pl pm|a/~3gpp.pic-bw-large plb|a/~mobius.plc plc|a/~pocketlearn plf|" +
	"a/pls+xml pls|a/~ctc-posml pml|i/png png|i/x-portable-anymap pnm|a/~macports.portpkg portpkg|" +
	"a/~ms-powerpoint pot pps ppt|a/~ms-powerpoint.template.macroenabled.12 potm|" +
	"a/~openxmlformats-officedocument.presentationml.template potx|a/~ms-powerpoint.addin.macroenabled.12 ppam|" +
	"a/~cups-ppd ppd|i/x-portable-pixmap ppm|a/~ms-powerpoint.slideshow.macroenabled.12 ppsm|" +
	"a/~openxmlformats-officedocument.presentationml.slideshow ppsx|" +
	"a/~ms-powerpoint.presentation.macroenabled.12 pptm|" +
	"a/~openxmlformats-officedocument.presentationml.presentation pptx|m/prc prc|a/~lotus-freelance pre|" +
	"a/pics-rules prf|a/provenance+xml provx|a/~3gpp.pic-bw-small psb|i/~adobe.photoshop psd|a/x-font-linux-psf psf|" +
	"a/pskc+xml pskcxml|i/prs.pti pti|a/~pvi.ptid1 ptid|a/x-mspublisher pub|a/~3gpp.pic-bw-var pvb|" +
	"a/~3m.post-it-notes pwn|u/~ms-playready.media.pya pya|v/~ms-playready.media.pyv pyv|a/~epson.quickanime qam|" +
	"a/~intu.qbo qbo|a/~intu.qfx qfx|a/~publishare-delta-tree qps|a/~quark.quarkxpress qwd qwt qxb qxd qxl qxt|" +
	"u/x-realaudio ra|u/x-pn-realaudio ram|a/raml+yaml raml|a/route-apd+xml rapd|a/x-rar-compressed rar|" +
	"i/x-cmu-raster ras|a/~ipunplugged.rcprofile rcprofile|a/~data-vision.rdz rdz|a/p2p-overlay+xml relo|" +
	"a/~businessobjects rep|a/x-dtbresource+xml res|i/x-rgb rgb|a/reginfo+xml rif|u/~rip rip|" +
	"a/x-research-info-systems ris|a/resource-lists+xml rl|i/~fujixerox.edmics-rlc rlc|" +
	"a/resource-lists-diff+xml rld|a/~rn-realmedia rm|u/x-pn-realaudio-plugin rmp|a/~jcp.javame.midlet-rms rms|" +
	"a/~rn-realmedia-vbr rmvb|a/relax-ng-compact-syntax rnc|a/xml rng xml xsd|a/rpki-roa roa|a/~cloanto.rp9 rp9|" +
	"a/x-redhat-package-manager rpm|a/~nokia.radio-presets rpss|a/~nokia.radio-preset rpst|a/sparql-query rq|" +
	"a/rls-services+xml rs|a/atsc-rsat+xml rsat|a/rsd+xml rsd|a/urc-ressheet+xml rsheet|a/rss+xml rss|t/rtf rtf|" +
	"t/richtext rtx|a/x-makeself run|a/route-usd+xml rusd|u/s3m s3m|a/~yamaha.smaf-audio saf|t/x-sass sass|" +
	"a/sbml+xml sbml|a/~ibm.secure-container sc|a/x-msschedule scd|a/~lotus-screencam scm|a/scvp-cv-request scq|" +
	"a/scvp-cv-response scs|t/x-scss scss|t/~curl.scurl scurl|a/~stardivision.draw sda|a/~stardivision.calc sdc|" +
	"a/~stardivision.impress sdd|a/~solent.sdkm+xml sdkd sdkm|a/sdp sdp|a/~stardivision.writer sdw vor|a/x-sea sea|" +
	"a/~seemail see|a/~sema sema|a/~semd semd|a/~semf semf|a/senml+xml senmlx|a/sensml+xml sensmlx|" +
	"a/java-serialized-object ser|a/set-payment-initiation setpay|a/set-registration-initiation setreg|" +
	"a/~hydrostatix.sof-data sfd-h|a/~spotfire.sfs sfs|t/x-sfv sfv|i/sgi sgi|a/~stardivision.writer-global sgl|" +
	"t/sgml sgm sgml|a/x-sh sh|a/x-shar shar|t/shex shex|a/shf+xml shf|i/x-mrsid-image sid|a/sieve sieve siv|" +
	"u/silk sil|a/~symbian.install sis sisx|a/x-stuffit sit|a/x-stuffitx sitx|a/~koan skd skm skp skt|" +
	"a/~ms-powerpoint.slide.macroenabled.12 sldm|a/~openxmlformats-officedocument.presentationml.slide sldx|" +
	"t/slim slim slm|a/route-s-tsid+xml sls|a/~epson.salt slt|a/~stepmania.stepchart sm|a/~stardivision.math smf|" +
	"a/smil+xml smi smil|v/x-smv smv|a/~stepmania.package smzip|a/x-font-snf snf|t/spdx spdx|" +
	"a/~yamaha.smaf-phrase spf|a/x-futuresplash spl|t/~in3d.spot spot|a/scvp-vp-response spp|a/scvp-vp-request spq|" +
	"a/x-sql sql|a/x-wais-source src|a/x-subrip srt|a/sru+xml sru|a/sparql-results+xml srx|a/ssdl+xml ssdl|" +
	"a/~kodak-descriptor sse|a/~epson.ssf ssf|a/ssml+xml ssml|a/~sailingtracker.track st|" +
	"a/~sun.xml.calc.template stc|a/~sun.xml.draw.template std|a/~wt.stf stf|a/~sun.xml.impress.template sti|" +
	"a/hyperstudio stk|m/stl stl|m/step+xml stpx|m/step-xml+zip stpxz|m/step+zip stpz|a/~pg.format str|" +
	"a/~sun.xml.writer.template stw|t/stylus styl stylus|t/~dvb.subtitle sub|a/~sus-calendar sus susp|" +
	"a/x-sv4cpio sv4cpio|a/x-sv4crc sv4crc|a/~dvb.service svc|a/~svd svd|i/svg+xml svg svgz|a/x-shockwave-flash swf|" +
	"a/~aristanetworks.swi swi|a/swid+xml swidtag|a/~sun.xml.calc sxc|a/~sun.xml.draw sxd|" +
	"a/~sun.xml.writer.global sxg|a/~sun.xml.impress sxi|a/~sun.xml.math sxm|a/~sun.xml.writer sxw|" +
	"a/x-t3vm-image t3|i/t38 t38|a/~mynfc taglet|a/~tao.intent-module-archive tao|i/~tencent.tap tap|a/x-tar tar|" +
	"a/~3gpp2.tcap tcap|a/x-tcl tcl tk|a/urc-targetdesc+xml td|a/~smart.teacher teacher|a/tei+xml tei teicorpus|" +
	"a/x-tex tex|a/x-texinfo texi texinfo|a/thraud+xml tfi|a/x-tex-tfm tfm|i/tiff-fx tfx|i/x-tga tga|" +
	"a/~ms-officetheme thmx|i/tiff tif tiff|a/~tmobile-livetv tmo|a/toml toml|t/yaml yaml yml|" +
	"a/x-bittorrent torrent|a/~groove-tool-template tpl|a/~trid.tpt tpt|a/~trueapp tra|a/trig trig|" +
	"a/x-msterminal trm|a/timestamped-data tsd|t/tab-separated-values tsv|f/collection ttc|f/ttf ttf|t/turtle ttl|" +
	"a/ttml+xml ttml|a/~simtech-mindmapper twd twds|a/~genomatix.tuxedo txd|a/~mobius.txf txf|m/u3d u3d|" +
	"g/global-delivery-status u8dsn|g/global-headers u8hdr|g/global-disposition-notification u8mdn|g/global u8msg|" +
	"a/ubjson ubj|a/~ufdl ufd ufdl|a/x-glulx ulx|a/~umajin umj|a/~unity unityweb|a/~uoml+xml uoml|" +
	"t/uri-list uri uris urls|m/~usdz+zip usdz|a/x-ustar ustar|a/~uiq.theme utz|t/x-uuencode uu|" +
	"u/~dece.audio uva uvva|a/~dece.data uvd uvf uvvd uvvf|i/~dece.graphic uvg uvi uvvg uvvi|v/~dece.hd uvh uvvh|" +
	"v/~dece.mobile uvm uvvm|v/~dece.pd uvp uvvp|v/~dece.sd uvs uvvs|a/~dece.ttml+xml uvt uvvt|v/~uvvu.mp4 uvu uvvu|" +
	"v/~dece.video uvv uvvv|a/~dece.unspecified uvvx uvx|a/~dece.zip uvvz uvz|a/x-virtualbox-vbox-extpack vbox-e|" +
	"a/x-virtualbox-vbox vbox|t/vcard vcard|a/x-cdlink vcd|t/x-vcard vcf|a/~groove-vcard vcg|t/x-vcalendar vcs|" +
	"a/~vcx vcx|a/x-virtualbox-vdi vdi|m/~sap.vds vds|a/x-virtualbox-vhd vhd|a/~visionary vis|v/~vivo viv|" +
	"a/x-virtualbox-vmdk vmdk|v/x-ms-vob vob|m/vrml vrml wrl|a/~visio vsd vss vst vsw|a/~vsf vsf|" +
	"i/~valve.source.texture vtf|t/vtt vtt|m/~vtu vtu|a/voicexml+xml vxml|a/x-doom wad|a/~sun.wadl+xml wadl|" +
	"a/webassembly wasm|u/x-wav wav|u/x-ms-wax wax|i/~wap.wbmp wbmp|a/~criticaltools.wbs+xml wbs|a/~wap.wbxml wbxml|" +
	"a/~ms-works wcm wdb wks wps|i/~ms-photo wdp|u/webm weba|a/x-web-app-manifest+json webapp|v/webm webm|" +
	"a/manifest+json webmanifest|i/webp webp|a/~pmi.widget wg|a/widget wgt|a/watcherinfo+xml wif|v/x-ms-wm wm|" +
	"u/x-ms-wma wma|a/x-ms-wmd wmd|i/wmf wmf|t/~wap.wml wml|a/~wap.wmlc wmlc|t/~wap.wmlscript wmls|" +
	"a/~wap.wmlscriptc wmlsc|v/x-ms-wmv wmv|v/x-ms-wmx wmx|f/woff woff|f/woff2 woff2|a/~wordperfect wpd|" +
	"a/~ms-wpl wpl|a/~wqd wqd|a/x-mswrite wri|g/~wfa.wsc wsc|a/wsdl+xml wsdl|a/wspolicy+xml wspolicy|" +
	"a/~webturbo wtb|v/x-ms-wvx wvx|m/~parasolid.transmit.binary x_b|m/~parasolid.transmit.text x_t|" +
	"m/x3d+xml x3d x3dz|m/x3d+fastinfoset x3db|m/x3d+binary x3dbz|m/x3d+vrml x3dv x3dvz|a/xaml+xml xaml|" +
	"a/x-silverlight-app xap|a/~xara xar|a/xcap-att+xml xav|a/x-ms-xbap xbap|a/~fujixerox.docuworks.binder xbd|" +
	"i/x-xbitmap xbm|a/xcap-caps+xml xca|a/calendar+xml xcs|a/xcap-diff+xml xdf|a/~syncml.dm+xml xdm|" +
	"a/~adobe.xdp+xml xdp|a/dssc+xml xdssc|a/~fujixerox.docuworks xdw|a/xcap-el+xml xel|a/xenc+xml xenc|" +
	"a/patch-ops-error+xml xer|a/~adobe.xfdf xfdf|a/~xfdl xfdl|a/xhtml+xml xht xhtml|i/~xiff xif|" +
	"a/~ms-excel xla xlc xlm xls xlt xlw|a/~ms-excel.addin.macroenabled.12 xlam|a/xliff+xml xlf|" +
	"a/~ms-excel.sheet.binary.macroenabled.12 xlsb|a/~ms-excel.sheet.macroenabled.12 xlsm|" +
	"a/~openxmlformats-officedocument.spreadsheetml.sheet xlsx|a/~ms-excel.template.macroenabled.12 xltm|" +
	"a/~openxmlformats-officedocument.spreadsheetml.template xltx|u/xm xm|a/xcap-ns+xml xns|a/~olpc-sugar xo|" +
	"a/xop+xml xop|a/x-xpinstall xpi|a/xproc+xml xpl|i/x-xpixmap xpm|a/~is-xpr xpr|a/~ms-xpsdocument xps|" +
	"a/~intercon.formnet xpw xpx|a/xslt+xml xsl xslt|a/~syncml+xml xsm|a/xspf+xml xspf|a/~mozilla.xul+xml xul|" +
	"i/x-xwindowdump xwd|c/x-xyz xyz|a/x-xz xz|a/yang yang|a/yin+xml yin|t/x-suse-ymp ymp|" +
	"a/x-zmachine z1 z2 z3 z4 z5 z6 z7 z8|a/~zzazz.deck+xml zaz|a/zip zip|a/~zul zir zirz|" +
	"a/~handheld-entertainment+xml zmm";

const MIME_PREFIXES: Record<string, string> = {
	a: "application/",
	t: "text/",
	i: "image/",
	v: "video/",
	u: "audio/",
	f: "font/",
	m: "model/",
	g: "message/",
	c: "chemical/",
	xc: "x-conference/",
};

let mimeByExt: Map<string, string> | undefined;

function extensionTable(): Map<string, string> {
	if (mimeByExt) return mimeByExt;
	const table = new Map<string, string>();
	for (const entry of MIME_TABLE.split("|")) {
		const parts = entry.split(" ");
		const encoded = parts[0];
		const slash = encoded.indexOf("/");
		const mime = MIME_PREFIXES[encoded.slice(0, slash)] + encoded.slice(slash + 1).replaceAll("~", "vnd.");
		for (let i = 1; i < parts.length; i++) table.set(parts[i], mime);
	}
	return (mimeByExt = table);
}

/** Bun's Compact::to_mime_type: canonical spellings (with charset) for a few table entries. */
function canonicalMime(mime: string): string {
	switch (mime) {
		case "application/webassembly":
			return "application/wasm";
		case "application/javascript":
		case "text/javascript":
		case "text/jsx":
			return "text/javascript;charset=utf-8";
		case "application/json":
			return "application/json;charset=utf-8";
		case "application/x-www-form-urlencoded":
			return "application/x-www-form-urlencoded;charset=UTF-8";
		case "text/css":
			return "text/css;charset=utf-8";
		case "text/html":
			return "text/html;charset=utf-8";
		case "text/plain":
			return "text/plain;charset=utf-8";
		default:
			return mime;
	}
}

function typeFromPath(path: string): string {
	const ext = extname(path).slice(1).toLowerCase();
	if (ext === "") return DEFAULT_TYPE;
	const mime = extensionTable().get(ext);
	return mime === undefined ? DEFAULT_TYPE : canonicalMime(mime);
}

function isValidBlobType(type: string): boolean {
	return !/[^\x20-\x7e]/.test(type);
}

/** Bun's MimeType::init for a user-supplied type: known types get their canonical charset form. */
function normalizeType(type: string): string {
	const lower = type.toLowerCase();
	const slash = lower.indexOf("/");
	if (slash < 0) return lower;
	if (slash === 0 || lower[0] === "*" || slash === lower.length - 1) return DEFAULT_TYPE;
	const category = lower.slice(0, slash);
	let sub = lower.slice(slash + 1);
	const semi = sub.indexOf(";");
	if (semi >= 0) sub = sub.slice(0, semi);
	if (category === "application") {
		if (sub === "json" || sub === "geo+json") return "application/json;charset=utf-8";
		if (sub === "octet-stream") return DEFAULT_TYPE;
		if (sub === "wasm") return "application/wasm";
	} else if (category === "text") {
		if (sub === "css" || sub === "html" || sub === "javascript" || sub === "plain") {
			return `text/${sub};charset=utf-8`;
		}
	}
	return lower;
}

// ── BunFile ──────────────────────────────────────────────────────────────────

interface FileState {
	/** Path as given (undefined for descriptor-backed files). */
	path: string | undefined;
	/** File descriptor, -1 for path-backed files. */
	fd: number;
	/** Byte window [start, end); end is Infinity for "to end of file". */
	start: number;
	end: number;
	type: string;
}

interface WriteOptions {
	createPath?: boolean;
	mode?: number;
}

interface SinkOptions {
	highWaterMark?: number;
}

type WriteInput =
	| string
	| Blob
	| BunFile
	| ArrayBuffer
	| SharedArrayBuffer
	| ArrayBufferView
	| Response
	| Request
	| ReadableStream<Uint8Array | string>
	| BlobPart[];

type PathInput = string | URL | ArrayBufferView | ArrayBuffer;

const kState = Symbol("BunFile.state");

interface TaggedFile {
	[kState]?: FileState;
}

/** The state of a BunFile, or undefined for any other value. */
function fileState(value: unknown): FileState | undefined {
	return value !== null && typeof value === "object" && kState in value ? (value as TaggedFile)[kState] : undefined;
}

class BunFile {
	constructor() {
		throw new TypeError("Illegal constructor");
	}

	get size(): number {
		const s = stateOf(this);
		return windowSize(s, statSize(s));
	}

	get type(): string {
		return stateOf(this).type;
	}

	/** The descriptor of a Bun.file(fd) file (used to pass it as spawn stdio). */
	get [Symbol.for("qbun.BunFile.fd")](): number | undefined {
		const fd = stateOf(this).fd;
		return fd >= 0 ? fd : undefined;
	}

	get name(): string | undefined {
		return stateOf(this).path;
	}

	get lastModified(): number {
		const s = stateOf(this);
		const st = statSyncOrUndefined(s);
		return st ? st.mtimeMs : Date.now();
	}

	async bytes(): Promise<Uint8Array> {
		return readBytes(stateOf(this));
	}

	async arrayBuffer(): Promise<ArrayBuffer> {
		const bytes = await readBytes(stateOf(this));
		if (bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength) return bytes.buffer as ArrayBuffer;
		return bytes.slice().buffer as ArrayBuffer;
	}

	async text(): Promise<string> {
		return decodeUtf8(await readBytes(stateOf(this)));
	}

	async json(): Promise<unknown> {
		return JSON.parse(decodeUtf8(await readBytes(stateOf(this))));
	}

	stream(): ReadableStream<Uint8Array> {
		return fileStream(stateOf(this));
	}

	slice(begin?: number | string, end?: number | string, contentType?: string): BunFile {
		const s = stateOf(this);
		let b: number | string | undefined = begin;
		let e: number | string | undefined = end;
		let type = contentType;
		if (typeof b === "string") {
			type = b;
			b = undefined;
			e = undefined;
		} else if (typeof e === "string") {
			type = e;
			e = undefined;
		}
		const relBegin = toInteger(b);
		const relEnd = e === undefined ? undefined : toInteger(e);
		let length = s.end === Infinity ? Infinity : s.end - s.start;
		if (relBegin < 0 || (relEnd !== undefined && relEnd < 0)) length = windowSize(s, statSize(s));
		const from = relBegin < 0 ? Math.max(length + relBegin, 0) : Math.min(relBegin, length);
		let to = relEnd === undefined ? length : relEnd < 0 ? Math.max(length + relEnd, 0) : Math.min(relEnd, length);
		if (to < from) to = from;
		const newType = typeof type === "string" && isValidBlobType(type) ? type.toLowerCase() : s.type;
		return makeFile({
			path: s.path,
			fd: s.fd,
			start: s.start + from,
			end: to === Infinity ? Infinity : s.start + to,
			type: newType,
		});
	}

	async exists(): Promise<boolean> {
		const s = stateOf(this);
		try {
			return !(await statOf(s)).isDirectory();
		} catch {
			return false;
		}
	}

	async stat(): Promise<Stats> {
		return statOf(stateOf(this));
	}

	async unlink(): Promise<void> {
		const s = stateOf(this);
		if (s.path === undefined) throw new TypeError("Cannot delete a file descriptor");
		await fs.promises.unlink(s.path);
	}

	async delete(): Promise<void> {
		return this.unlink();
	}

	write(data: WriteInput, options?: WriteOptions): Promise<number> {
		stateOf(this);
		return write(this, data, options);
	}

	writer(options?: SinkOptions): FileSink {
		const s = stateOf(this);
		if (s.path === undefined) return new FileSink(s.fd, false, options);
		const fd = fs.openSync(s.path, fs.constants.O_WRONLY | fs.constants.O_CREAT, 0o664);
		return new FileSink(fd, true, options);
	}
}

let blobLinked = false;

/** Chain BunFile under the global Blob and expose its bytes to Blob internals. */
function linkBlob(): void {
	blobLinked = true;
	Object.setPrototypeOf(BunFile.prototype, Blob.prototype);
	Object.setPrototypeOf(BunFile, Blob);
	// node:buffer's Blob keeps its bytes under a private symbol; Blob parts and
	// getBlobBytes() read it, so a BunFile answers with a synchronous read.
	const dataKey = Object.getOwnPropertySymbols(new Blob([])).find((sym) => sym.description === "Blob.data");
	if (dataKey !== undefined) {
		Object.defineProperty(BunFile.prototype, dataKey, {
			configurable: true,
			get(this: BunFile): Uint8Array {
				return readBytesSync(stateOf(this));
			},
		});
	}
}

function stateOf(value: unknown): FileState {
	const s = fileState(value);
	if (s === undefined) throw codedError("ERR_INVALID_THIS", 'Value of "this" must be of type BunFile', TypeError);
	if (!blobLinked) linkBlob();
	return s;
}

function makeFile(state: FileState): BunFile {
	const f = Object.create(BunFile.prototype) as BunFile;
	Object.defineProperty(f, kState, { value: state });
	return f;
}

function toInteger(v: number | string | undefined): number {
	if (v === undefined) return 0;
	const n = Number(v);
	return Number.isNaN(n) ? 0 : Math.trunc(n);
}

function windowSize(s: FileState, total: number): number {
	const end = Math.min(s.end, total);
	return end > s.start ? end - s.start : 0;
}

function statSyncOrUndefined(s: FileState): Stats | undefined {
	try {
		return s.path !== undefined ? fs.statSync(s.path, { throwIfNoEntry: false }) : fs.fstatSync(s.fd);
	} catch {
		return undefined;
	}
}

/** Size of a regular file; 0 when missing or not a regular file. */
function statSize(s: FileState): number {
	const st = statSyncOrUndefined(s);
	return st !== undefined && st.isFile() ? st.size : 0;
}

function statOf(s: FileState): Promise<Stats> {
	if (s.path !== undefined) return fs.promises.stat(s.path);
	const { promise, resolve, reject } = Promise.withResolvers<Stats>();
	fs.fstat(s.fd, (err, st) => (err ? reject(err) : resolve(st)));
	return promise;
}

function decodeUtf8(bytes: Uint8Array): string {
	const start = bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf ? 3 : 0;
	return textNative.decode(bytes, "utf8", start, bytes.length);
}

function asUint8(view: ArrayBufferView): Uint8Array {
	return new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
}

function concat(chunks: Uint8Array[], total: number): Uint8Array {
	if (chunks.length === 1 && chunks[0].byteLength === total) return chunks[0];
	const out = new Uint8Array(total);
	let off = 0;
	for (const c of chunks) {
		out.set(c, off);
		off += c.byteLength;
	}
	return out;
}

/** fs.read on a descriptor; a non-blocking descriptor (TTY/pipe) is polled until data arrives. */
function fdRead(fd: number, buf: Uint8Array, position: number | null): Promise<number> {
	const { promise, resolve, reject } = Promise.withResolvers<number>();
	const attempt = (): void => {
		fs.read(fd, buf, 0, buf.byteLength, position, (err, n) => {
			if (err) {
				const code = "code" in err ? err.code : undefined;
				if (code === "EAGAIN" || code === "EWOULDBLOCK") setTimeout(attempt, 10);
				else reject(err);
				return;
			}
			resolve(n);
		});
	};
	attempt();
	return promise;
}

async function readFdRange(fd: number, start: number, end: number): Promise<Uint8Array> {
	const sequential = start === 0 && end === Infinity;
	const chunks: Uint8Array[] = [];
	let total = 0;
	let pos = start;
	while (pos < end) {
		const want = Math.min(CHUNK, end - pos);
		const buf = new Uint8Array(want);
		const n = await fdRead(fd, buf, sequential ? null : pos);
		if (n === 0) break;
		chunks.push(n === want ? buf : buf.subarray(0, n));
		total += n;
		pos += n;
	}
	return concat(chunks, total);
}

async function readHandleRange(handle: FileHandle, start: number, end: number): Promise<Uint8Array> {
	const chunks: Uint8Array[] = [];
	let total = 0;
	let pos = start;
	while (pos < end) {
		const want = Math.min(end - pos, 1024 * 1024);
		const buf = new Uint8Array(want);
		const { bytesRead } = await handle.read(buf, 0, want, pos);
		if (bytesRead === 0) break;
		chunks.push(bytesRead === want ? buf : buf.subarray(0, bytesRead));
		total += bytesRead;
		pos += bytesRead;
	}
	return concat(chunks, total);
}

async function readBytes(s: FileState): Promise<Uint8Array> {
	if (s.path === undefined) return readFdRange(s.fd, s.start, s.end);
	if (s.start === 0 && s.end === Infinity) return asUint8(await fs.promises.readFile(s.path));
	const handle = await fs.promises.open(s.path, "r");
	try {
		return await readHandleRange(handle, s.start, s.end);
	} finally {
		await handle.close();
	}
}

function readFdRangeSync(fd: number, start: number, end: number): Uint8Array {
	const sequential = start === 0 && end === Infinity;
	const chunks: Uint8Array[] = [];
	let total = 0;
	let pos = start;
	while (pos < end) {
		const want = Math.min(CHUNK, end - pos);
		const buf = new Uint8Array(want);
		const n = fs.readSync(fd, buf, 0, want, sequential ? null : pos);
		if (n === 0) break;
		chunks.push(n === want ? buf : buf.subarray(0, n));
		total += n;
		pos += n;
	}
	return concat(chunks, total);
}

function readBytesSync(s: FileState): Uint8Array {
	if (s.path === undefined) return readFdRangeSync(s.fd, s.start, s.end);
	if (s.start === 0 && s.end === Infinity) return asUint8(fs.readFileSync(s.path));
	const fd = fs.openSync(s.path, "r");
	try {
		return readFdRangeSync(fd, s.start, s.end);
	} finally {
		fs.closeSync(fd);
	}
}

function fileStream(s: FileState): ReadableStream<Uint8Array> {
	const sequential = s.path === undefined && s.start === 0 && s.end === Infinity;
	let handle: FileHandle | undefined;
	let pos = s.start;
	const release = async (): Promise<void> => {
		const h = handle;
		handle = undefined;
		if (h !== undefined) await h.close();
	};
	return new ReadableStream<Uint8Array>(
		{
			async pull(controller: ReadableStreamDefaultController<Uint8Array>): Promise<void> {
				try {
					if (pos >= s.end) {
						await release();
						controller.close();
						return;
					}
					const want = Math.min(CHUNK, s.end - pos);
					const buf = new Uint8Array(want);
					let n: number;
					if (s.path === undefined) {
						n = await fdRead(s.fd, buf, sequential ? null : pos);
					} else {
						handle ??= await fs.promises.open(s.path, "r");
						n = (await handle.read(buf, 0, want, pos)).bytesRead;
					}
					if (n === 0) {
						await release();
						controller.close();
						return;
					}
					pos += n;
					controller.enqueue(n === want ? buf : buf.slice(0, n));
				} catch (err) {
					await release().catch(() => {});
					controller.error(err);
				}
			},
			async cancel(): Promise<void> {
				await release();
			},
		},
		{ highWaterMark: 0 },
	);
}

function isUrlLike(v: unknown): v is URL {
	return (
		v !== null &&
		typeof v === "object" &&
		"href" in v &&
		typeof v.href === "string" &&
		"protocol" in v &&
		typeof v.protocol === "string"
	);
}

function toPath(v: unknown, name: string): string {
	if (typeof v === "string") return v;
	if (isUrlLike(v)) {
		return fileURLToPath(v);
	}
	if (ArrayBuffer.isView(v)) {
		const bytes = asUint8(v);
		return textNative.decode(bytes, "utf8", 0, bytes.length);
	}
	if (v instanceof ArrayBuffer) {
		const bytes = new Uint8Array(v);
		return textNative.decode(bytes, "utf8", 0, bytes.length);
	}
	throw invalidArgType(name, "of type string or an instance of URL, Buffer or number", v);
}

function validateFd(fd: number, name: string): void {
	if (!Number.isInteger(fd) || fd < 0 || fd > MAX_FD) throw outOfRange(name, `>= 0 && <= ${MAX_FD}`, fd);
}

/**
 * Bun.file(): a lazy reference to a path, `file://` URL or file descriptor.
 * `options.type` overrides the MIME type derived from the extension.
 */
export function file(path: PathInput | number, options?: { type?: string }): BunFile {
	let p: string | undefined;
	let fd = -1;
	if (typeof path === "number") {
		validateFd(path, "fd");
		fd = path;
	} else {
		p = toPath(path, "path");
	}
	const given = options?.type;
	const type =
		typeof given === "string" && given !== "" && isValidBlobType(given)
			? normalizeType(given)
			: p !== undefined
				? typeFromPath(p)
				: DEFAULT_TYPE;
	if (!blobLinked) linkBlob();
	return makeFile({ path: p, fd, start: 0, end: Infinity, type });
}

/** Bun.stdin / Bun.stdout / Bun.stderr. */
export const stdin: BunFile = makeFile({ path: undefined, fd: 0, start: 0, end: Infinity, type: DEFAULT_TYPE });
export const stdout: BunFile = makeFile({ path: undefined, fd: 1, start: 0, end: Infinity, type: DEFAULT_TYPE });
export const stderr: BunFile = makeFile({ path: undefined, fd: 2, start: 0, end: Infinity, type: DEFAULT_TYPE });

// ── FileSink ─────────────────────────────────────────────────────────────────

function chunkBytes(chunk: unknown, name: string): Uint8Array {
	if (typeof chunk === "string") return textNative.utf8Encode(chunk);
	if (
		chunk instanceof ArrayBuffer ||
		(typeof SharedArrayBuffer !== "undefined" && chunk instanceof SharedArrayBuffer)
	) {
		return new Uint8Array(chunk);
	}
	if (ArrayBuffer.isView(chunk)) return asUint8(chunk);
	throw invalidArgType(
		name,
		"of type string or an instance of ArrayBuffer, SharedArrayBuffer or ArrayBufferView",
		chunk,
	);
}

/**
 * Incremental writer over a file descriptor (Bun.file(...).writer()).
 * Writes are buffered up to `highWaterMark` and flushed synchronously, either
 * when the buffer fills, on flush()/end(), or automatically at the end of the
 * current microtask checkpoint. Writes to fds 0-2 go straight to the descriptor.
 * Every operation completes synchronously, so ref()/unref() have nothing to hold.
 */
export class FileSink {
	#fd: number;
	#owned: boolean;
	#direct: boolean;
	#highWaterMark = DEFAULT_SINK_HWM;
	#chunks: Uint8Array[] = [];
	#buffered = 0;
	#closed = false;
	#scheduled = false;
	#error: unknown = undefined;
	#hasError = false;

	/** `owned`: close the descriptor on end(). */
	constructor(fd: number, owned = false, options?: SinkOptions) {
		validateFd(fd, "fd");
		this.#fd = fd;
		this.#owned = owned;
		this.#direct = fd <= 2;
		this.start(options);
	}

	start(options?: SinkOptions): void {
		const hwm = options?.highWaterMark;
		if (typeof hwm === "number" && hwm > 0 && Number.isFinite(hwm)) this.#highWaterMark = Math.trunc(hwm) || 1;
	}

	write(chunk: string | ArrayBufferView | ArrayBuffer | SharedArrayBuffer): number {
		this.#check();
		if (this.#direct) {
			if (typeof chunk === "string") return chunk.length === 0 ? 0 : coreIo.writeSync(this.#fd, chunk);
			const bytes = chunkBytes(chunk, "chunk");
			return bytes.byteLength === 0 ? 0 : coreIo.writeSync(this.#fd, bytes);
		}
		// Strings encode into a fresh buffer; caller-owned bytes are copied since
		// they may be reused before the buffer is flushed.
		const bytes = typeof chunk === "string" ? textNative.utf8Encode(chunk) : chunkBytes(chunk, "chunk").slice();
		const n = bytes.byteLength;
		if (n === 0) return 0;
		this.#chunks.push(bytes);
		this.#buffered += n;
		if (this.#buffered >= this.#highWaterMark) this.#flushBuffer();
		else this.#schedule();
		return n;
	}

	flush(): number {
		this.#check();
		return this.#flushBuffer();
	}

	end(error?: Error): number {
		if (this.#closed) return 0;
		let n = 0;
		try {
			if (error === undefined) {
				this.#throwPendingError();
				n = this.#flushBuffer();
			}
		} finally {
			this.#closed = true;
			this.#chunks = [];
			this.#buffered = 0;
			if (this.#owned) fs.closeSync(this.#fd);
		}
		return n;
	}

	ref(): void {}

	unref(): void {}

	#throwPendingError(): void {
		if (!this.#hasError) return;
		const err = this.#error;
		this.#hasError = false;
		this.#error = undefined;
		throw err;
	}

	#check(): void {
		this.#throwPendingError();
		if (this.#closed) throw codedError("ERR_INVALID_STATE", "Invalid state: FileSink is closed", TypeError);
	}

	#schedule(): void {
		if (this.#scheduled) return;
		this.#scheduled = true;
		queueMicrotask(() => {
			this.#scheduled = false;
			if (this.#closed) return;
			try {
				this.#flushBuffer();
			} catch (err) {
				this.#error = err;
				this.#hasError = true;
			}
		});
	}

	#flushBuffer(): number {
		const n = this.#buffered;
		if (n === 0) return 0;
		const data = concat(this.#chunks, n);
		this.#chunks = [];
		this.#buffered = 0;
		coreIo.writeSync(this.#fd, data);
		return n;
	}
}

// ── Bun.write ────────────────────────────────────────────────────────────────

interface Destination {
	path: string | undefined;
	fd: number;
}

function toDestination(dest: unknown): Destination {
	const s = fileState(dest);
	if (s !== undefined) return { path: s.path, fd: s.fd };
	if (typeof dest === "number") {
		validateFd(dest, "fd");
		return { path: undefined, fd: dest };
	}
	if (dest !== null && typeof dest === "object" && "arrayBuffer" in dest && typeof dest.arrayBuffer === "function") {
		throw new TypeError("Cannot write to a Blob backed by bytes, which are always read-only");
	}
	return { path: toPath(dest, "destination"), fd: -1 };
}

function isReadableStream(v: unknown): v is ReadableStream<unknown> {
	return v !== null && typeof v === "object" && "getReader" in v && typeof v.getReader === "function";
}

/** Run `op`; on ENOENT create the parent directory (unless createPath is false) and retry once. */
async function withCreatePath<T>(path: string, options: WriteOptions | undefined, op: () => Promise<T>): Promise<T> {
	try {
		return await op();
	} catch (err) {
		const code = err !== null && typeof err === "object" && "code" in err ? err.code : undefined;
		if (options?.createPath === false || code !== "ENOENT") throw err;
		try {
			await fs.promises.mkdir(dirname(path), { recursive: true });
		} catch {
			// The open()'s ENOENT is what Bun reports when the mkdir fails.
			throw err;
		}
		return op();
	}
}

async function writeData(
	dest: Destination,
	data: string | Uint8Array,
	options: WriteOptions | undefined,
): Promise<number> {
	if (dest.path === undefined) {
		if (typeof data === "string") return data.length === 0 ? 0 : coreIo.writeSync(dest.fd, data);
		return data.byteLength === 0 ? 0 : coreIo.writeSync(dest.fd, data);
	}
	const path = dest.path;
	const mode = options?.mode;
	const writeOptions = mode === undefined ? {} : { mode };
	await withCreatePath(path, options, () => fs.promises.writeFile(path, data, writeOptions));
	if (mode !== undefined) await fs.promises.chmod(path, mode);
	return typeof data === "string" ? textNative.byteLengthUtf8(data) : data.byteLength;
}

async function writeStream(
	dest: Destination,
	stream: ReadableStream<unknown>,
	options: WriteOptions | undefined,
): Promise<number> {
	const reader = stream.getReader();
	let total = 0;
	if (dest.path === undefined) {
		try {
			for (;;) {
				const { done, value } = await reader.read();
				if (done) break;
				const bytes = chunkBytes(value, "chunk");
				if (bytes.byteLength > 0) total += coreIo.writeSync(dest.fd, bytes);
			}
		} catch (err) {
			reader.cancel(err).catch(() => {});
			throw err;
		}
		return total;
	}
	const path = dest.path;
	const mode = options?.mode;
	const handle = await withCreatePath(path, options, () => fs.promises.open(path, "w", mode ?? 0o666));
	try {
		for (;;) {
			const { done, value } = await reader.read();
			if (done) break;
			const bytes = chunkBytes(value, "chunk");
			let off = 0;
			while (off < bytes.byteLength) {
				const { bytesWritten } = await handle.write(bytes, off, bytes.byteLength - off);
				off += bytesWritten;
			}
			total += bytes.byteLength;
		}
	} catch (err) {
		reader.cancel(err).catch(() => {});
		throw err;
	} finally {
		await handle.close();
	}
	if (mode !== undefined) await fs.promises.chmod(path, mode);
	return total;
}

async function inputBytes(input: unknown): Promise<string | Uint8Array | ReadableStream<unknown>> {
	if (typeof input === "string") return input;
	const src = fileState(input);
	if (src !== undefined) return readBytes(src);
	if (
		input instanceof ArrayBuffer ||
		(typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer)
	) {
		return new Uint8Array(input);
	}
	if (ArrayBuffer.isView(input)) return asUint8(input);
	if (Array.isArray(input)) {
		const blob = new Blob(input as BlobPart[]);
		return new Uint8Array(await blob.arrayBuffer());
	}
	if (isReadableStream(input)) return input;
	if (input !== null && typeof input === "object") {
		// Blob, Response, Request (and Bun.Archive, which only has bytes()).
		const body = input as { arrayBuffer?: () => Promise<ArrayBuffer>; bytes?: () => Promise<Uint8Array> };
		if (typeof body.arrayBuffer === "function") return new Uint8Array(await body.arrayBuffer());
		if (typeof body.bytes === "function") return asUint8(await body.bytes());
	}
	throw invalidArgType(
		"data",
		"of type string or an instance of Blob, ArrayBuffer, TypedArray, DataView, Response, Request or ReadableStream",
		input,
	);
}

/**
 * Bun.write(destination, data, options?): write `data` to a path, URL, BunFile or
 * fd, resolving to the number of bytes written. Paths are truncated/created
 * (missing parent directories are created unless `createPath: false`).
 */
export function write(
	destination: PathInput | BunFile | number,
	data: WriteInput,
	options?: WriteOptions,
): Promise<number> {
	const dest = toDestination(destination);
	const mode = options?.mode;
	if (mode !== undefined && (typeof mode !== "number" || !Number.isInteger(mode) || mode < 0 || mode > 0o777)) {
		throw outOfRange("mode", ">= 0 && <= 511", mode);
	}
	return (async (): Promise<number> => {
		const payload = await inputBytes(data);
		if (isReadableStream(payload)) return writeStream(dest, payload, options);
		return writeData(dest, payload, options);
	})();
}

// ── Bun.mmap ─────────────────────────────────────────────────────────────────

/**
 * Bun.mmap(path, { offset, size }): the file's bytes as a Uint8Array.
 * qbun returns a private copy read at call time, not a shared mapping: writes
 * to the array do not reach the file and later file changes are not visible.
 */
export function mmap(
	path: PathInput,
	opts?: { shared?: boolean; sync?: boolean; size?: number; offset?: number },
): Uint8Array {
	const bytes = asUint8(fs.readFileSync(toPath(path, "path")));
	const offset = opts?.offset ?? 0;
	if (!Number.isInteger(offset) || offset < 0 || offset > bytes.byteLength) {
		throw outOfRange("offset", `>= 0 && <= ${bytes.byteLength}`, offset);
	}
	const available = bytes.byteLength - offset;
	const size = opts?.size === undefined ? available : Math.min(Math.max(Math.trunc(opts.size), 0), available);
	return bytes.subarray(offset, offset + size);
}
