/*! Estimate btag efficiencies.
This file is part of https://github.com/hh-italian-group/hh-bbtautau. */
#include <boost/format.hpp>
#include <vector>
#include <set>
#include <map>

#include "AnalysisTools/Run/include/program_main.h" 
#include "h-tautau/Analysis/include/EventTuple.h"
#include "AnalysisTools/Core/include/AnalyzerData.h"
#include "h-tautau/Analysis/include/AnalysisTypes.h"
#include "h-tautau/Cuts/include/Btag_2016.h"
#include "h-tautau/Cuts/include/hh_bbtautau_2016.h"
#include "AnalysisTools/Core/include/Tools.h"
#include "AnalysisTools/Core/include/TextIO.h"

struct Arguments { // list of all program arguments
    REQ_ARG(std::string, output_file); 
    REQ_ARG(std::vector<std::string>, input_file); 
    OPT_ARG(bool,apply_tau_id_cut, true); 
    OPT_ARG(bool,apply_pu_id_cut,false);
};

class BTagData : public root_ext::AnalyzerData {
public:
    explicit BTagData(std::shared_ptr<TFile> _outputFile, const std::string& directoryName = "") :
        AnalyzerData(_outputFile, directoryName)
        {
            static const std::vector<double> x_bins = {20,30,40,60,100,150,200,300,650,1000};
            static const std::vector<double> y_bins = {0,0.6,1.2,2.1,2.4}; 
            h2.SetMasterHist(x_bins, y_bins);
            eff.SetMasterHist(x_bins, y_bins);
        }
        ANA_DATA_ENTRY(TH2D, h2)
        ANA_DATA_ENTRY(TH2D, eff)
};

class BTagEfficiency { 
public:
    using Event = ntuple::Event;
    using EventTuple = ntuple::EventTuple;
    using EventEnergyScale = analysis::EventEnergyScale;
    using Channel = analysis::Channel; 
    using DiscriminatorWP = analysis::DiscriminatorWP; 



    BTagEfficiency(const Arguments& _args) : args(_args), 
    outfile(root_ext::CreateRootFile(args.output_file())), anaData(outfile)
    {
    }
    void Run()
    {
        static const std::set<std::string> channels = { ToString(Channel::ETau), ToString(Channel::MuTau),
            ToString(Channel::TauTau), ToString(Channel::MuMu) };
        std::string channel_all = "all";
        std::set<std::string> channel_names = channels;
        channel_names.insert(channel_all);
        static const std::map<std::string, double> btag_working_points = { { "L", cuts::btag_2016::CSVv2L },
            { "M", cuts::btag_2016::CSVv2M }, { "T", cuts::btag_2016::CSVv2T} };
        static const std::string btag_wp_all = "all";
        static const std::map<int, std::string> flavours = { { 5, "b" }, { 4, "c" }, { 0, "udsg" } };
        static const std::string flavour_all = "all";
        std::set<std::string> flavour_names = analysis::tools::collect_map_values(flavours);
        flavour_names.insert(flavour_all);
        static const std::string num = "Num", denom = "Denom", eff = "Eff" ;
       


        for(const auto& channel : channels) {
            for (const auto& name : args.input_file()){
                std::shared_ptr<TFile> in_file(root_ext::OpenRootFile(name));
                std::shared_ptr<EventTuple> tuple;
                try {
                    tuple = ntuple::CreateEventTuple(channel, in_file.get(), true, ntuple::TreeState::Full);
                } catch(std::exception&) {
                    std::cerr << "WARNING: tree "<<channel<<" not found in file '"<< std::endl;
                    continue;
                }

                for(const Event& event : *tuple){
                    const EventEnergyScale es = static_cast<EventEnergyScale>(event.eventEnergyScale);
                    if (es != EventEnergyScale::Central || event.jets_p4.size() < 2 || event.extraelec_veto
                            || event.extramuon_veto
                            || std::abs(event.jets_p4.at(0).eta()) >= cuts::btag_2016::eta
                            || std::abs(event.jets_p4.at(1).eta()) >= cuts::btag_2016::eta) continue;

                    auto bb = event.jets_p4.at(0) + event.jets_p4.at(1);
                    if (!cuts::hh_bbtautau_2016::hh_tag::IsInsideEllipse(event.SVfit_p4.mass(),bb.mass())) continue;

                    if ((event.q_1+event.q_2) != 0) continue;

                    static const std::string tau_iso_disc = "byMediumIsolationMVArun2v1DBoldDMwLT";
                    static const uint32_t tau_iso_disc_hash = analysis::tools::hash(tau_iso_disc);
                    static const double tau_iso_cut = 0.5;
                    if(args.apply_tau_id_cut() && 
                            (!PassTauIdCut(event.tauId_keys_1, event.tauId_values_1, tau_iso_disc_hash, tau_iso_cut) ||
                             !PassTauIdCut(event.tauId_keys_2, event.tauId_values_2, tau_iso_disc_hash, tau_iso_cut))) continue;   

                    for (size_t i=0; i<2; i++){
                        const auto& jet = event.jets_p4.at(i);

                        //PU correction
                        if(args.apply_pu_id_cut()){
                            double jet_mva = event.jets_mva.at(i);
                            if(!PassJetPuId(jet.Pt(),jet_mva,DiscriminatorWP::Loose)) continue;
                        }
			
                        double jet_csv = event.jets_csv.at(i);
                        int jet_hadronFlavour = event.jets_hadronFlavour.at(i);
                        const std::string& jet_flavour = flavours.at(jet_hadronFlavour);

                        anaData.h2(denom, flavour_all, btag_wp_all, channel).Fill(jet.Pt(), std::abs(jet.Eta()));
                        anaData.h2(denom, flavour_all, btag_wp_all, channel_all).Fill(jet.Pt(), std::abs(jet.Eta()));
                        anaData.h2(denom, jet_flavour, btag_wp_all, channel).Fill(jet.Pt(), std::abs(jet.Eta()));
                        anaData.h2(denom, jet_flavour, btag_wp_all, channel_all).Fill(jet.Pt(), std::abs(jet.Eta()));

                        for(const auto& btag_wp : btag_working_points) {
                            if(btag_wp.second > jet_csv){
                                anaData.h2(num, flavour_all, btag_wp.first, channel).Fill(jet.Pt(), std::abs(jet.Eta()));
                                anaData.h2(num, flavour_all, btag_wp.first, channel_all).Fill(jet.Pt(), std::abs(jet.Eta()));
                                anaData.h2(num, jet_flavour, btag_wp.first, channel).Fill(jet.Pt(), std::abs(jet.Eta()));
                                anaData.h2(num, jet_flavour, btag_wp.first, channel_all).Fill(jet.Pt(), std::abs(jet.Eta()));
                            }
                        }
                    }//end loop on jets 
                }//end loop on events
            }// end loop on files
        }//end loop on channel
        for(const auto& channel : channel_names){
            for(const auto& btag_wp : btag_working_points) {
                for(const auto& jet_flavour : flavour_names){
                    anaData.eff(jet_flavour,btag_wp.first,channel).CopyContent(anaData.h2(num,jet_flavour,
                                btag_wp.first,channel));
                    anaData.eff(jet_flavour,btag_wp.first,channel).Divide(&anaData.h2(denom,jet_flavour,
                                btag_wp_all,channel));
                }
            }
        }
    }
private:
    Arguments args;
    std::shared_ptr<TFile> outfile;
    BTagData anaData;

    static bool PassTauIdCut(const std::vector<uint32_t>& tauId_keys, const std::vector<float>& tauId_values,
            uint32_t discriminator_name_hash, float cut_value)
    {
        for(size_t n = 0; n < tauId_keys.size(); ++n) {
            if(tauId_keys.at( n ) == discriminator_name_hash) return tauId_values.at( n ) > cut_value;
        }
        return true;
    }

    static bool PassJetPuId(double pt, double mva, DiscriminatorWP wp)
    {
        //PU Id cuts
        //https://github.com/cms-sw/cmssw/blob/CMSSW_8_0_X/RecoJets/JetProducers/python/PileupJetIDCutParams_cfi.py#L31
        static const std::map<DiscriminatorWP, std::vector<std::pair<double,double> > > puId_working_points = 
        { { DiscriminatorWP::Tight, { {30,0.26}, {50,0.62} } }, { DiscriminatorWP::Medium, { {30,-0.49}, {50,-0.06} } },
            { DiscriminatorWP::Loose, { {30,-0.96}, {50,-0.92} } } };
        bool result = false; 
        for(const auto& working_point: puId_working_points){
             if (working_point.first != wp) continue;
             for ( const auto& cut_values: working_point.second){
                if (pt< cut_values.first){
                    if (mva < cut_values.second){
                        result =true;
                        break;
                    }
                }
             } 
        }
        return result;
    }
};

PROGRAM_MAIN(BTagEfficiency, Arguments) // definition of the main program function