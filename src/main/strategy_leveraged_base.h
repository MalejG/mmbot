/*
 * strategy_leveraged_base.h
 *
 *  Created on: 9. 5. 2020
 *      Author: ondra
 */

#ifndef SRC_MAIN_STRATEGY_LEVERAGED_BASE_H_
#define SRC_MAIN_STRATEGY_LEVERAGED_BASE_H_
#include <memory>




template<typename Calc>
class Strategy_Leveraged: public IStrategy {
public:

	using TCalc = Calc;
	using PCalc = std::shared_ptr<Calc>;

	struct Config {
		double power;
		double asym = 0;
		double max_loss = 0;
		double reduction = 0;
		double external_balance = 0;
		double powadj = 0;
		double dynred = 0;
		double initboost = 0;
		bool detect_trend = false;
		bool recalc_keep_neutral =false;
		bool longonly = false;
		bool fastclose = false;
		bool slowopen = false;
		bool reinvest_profit = false;
//		int preference = 0;
	};

	using PConfig = std::shared_ptr<const Config>;


	struct State {
		double neutral_price = 0;
		double last_price =0;
		double position = 0;
		double bal = 0;
		double val = 0;
		double redbal = 0;
		double power = 0;
		double neutral_pos = 0;
		long trend_cntr = 0;
	};

	Strategy_Leveraged(const PCalc &calc, const PConfig &cfg, State &&st);
	Strategy_Leveraged(const PCalc &calc, const PConfig &cfg);

	virtual bool isValid() const override;
	virtual PStrategy  onIdle(const IStockApi::MarketInfo &minfo, const IStockApi::Ticker &curTicker, double assets, double currency) const override;
	virtual std::pair<OnTradeResult,PStrategy > onTrade(const IStockApi::MarketInfo &minfo, double tradePrice, double tradeSize, double assetsLeft, double currencyLeft) const override;;
	virtual json::Value exportState() const override;
	virtual PStrategy importState(json::Value src,const IStockApi::MarketInfo &minfo) const override;
	virtual OrderData getNewOrder(const IStockApi::MarketInfo &minfo,  double cur_price,double new_price, double dir, double assets, double currency, bool rej) const override;
	virtual MinMax calcSafeRange(const IStockApi::MarketInfo &minfo, double assets, double currencies) const override;
	virtual double getEquilibrium(double assets) const override;
	virtual PStrategy reset() const override;
	virtual json::Value dumpStatePretty(const IStockApi::MarketInfo &minfo) const override;
	virtual std::string_view getID() const override {return id;}
	virtual BudgetInfo getBudgetInfo() const override;
	virtual double calcCurrencyAllocation(double) const override;
	virtual std::optional<BudgetExtraInfo> getBudgetExtraInfo(double price, double currency) const {
		return std::optional<BudgetExtraInfo>();
	}


	static std::string_view id;

protected:
	PCalc calc;
	PConfig cfg;
	State st;
	mutable std::optional<MinMax> rootsCache;


	static PStrategy init(const PCalc &calc, const PConfig &cfg, double price, double pos, double currency, const IStockApi::MarketInfo &minfo);
	double calcPosition(double price) const;

	MinMax calcRoots() const;
	double adjNeutral(double price, double value) const;

	double calcMaxLoss() const;


	double calcNewNeutralFromProfit(double profit, double price, double reduction) const;

private:
	static void recalcPower(const PCalc &calc, const PConfig &cfg, State &nwst) ;
	static void recalcNeutral(const PCalc &calc, const PConfig &cfg, State &nwst) ;
	json::Value storeCfgCmp() const;
	static void recalcNewState(const PCalc &calc, const PConfig &cfg, State &nwst);
	double calcAsym() const;
	static double calcAsym(const PConfig &cfg, const State &st) ;
	static double trendFactor(const State &st);
	static std::pair<double,double> getBalance(const Config &cfg, bool leveraged, double price, double assets, double currency);
	virtual double calcInitialPosition(const IStockApi::MarketInfo &minfo, double price, double assets, double currency) const override;

};

#endif /* SRC_MAIN_STRATEGY_LEVERAGED_BASE_H_ */
